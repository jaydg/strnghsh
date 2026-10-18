// Altered C++20 reimplementation of foonathan/string_id.
// Copyright (C) 2014-2015 Jonathan Müller <jonathanmueller.dev@gmail.com>
// Distributed under the zlib license; see LICENSE.

export module strnghsh;
import std;

/// Lightweight string identifiers with optional text storage.
export namespace strnghsh
{

/// The stable, platform-independent 64-bit hash representation.
using hash_type = std::uint64_t;

/// FNV-1a over unsigned bytes, including embedded NULs; seed permits prefix hashing.
/// This is a non-cryptographic hash. Raw hashes cannot detect collisions.
[[nodiscard]] constexpr hash_type hash(std::string_view text,
                                       hash_type seed = 14695981039346656037ULL) noexcept
{
    for (unsigned char byte : text) {
        seed = (seed ^ byte) * 1099511628211ULL;
    }

    return seed;
}

/// Compile-time string hash literals.
namespace literals
{
/// Hash all bytes of a string literal for comparisons and switch labels.
[[nodiscard]] consteval hash_type operator""_id(const char* text, std::size_t size) noexcept
{
    return hash({text, size});
}
}

/// Result of attempting to store a hash/text pair.
enum class insert_status {
    new_string, ///< A new pair was stored (or storage is disabled).
    old_string, ///< The same pair was already present.
    collision   ///< The hash already belongs to different text; nothing changed.
};

/// Stable-address database interface. Must outlive its identifiers and returned views.
/// Implementations must own inserted text, never erase/change it, and preserve views.
/// @par Thread safety
/// Custom implementations define their own synchronization. If not internally synchronized,
/// callers must serialize all access, including through string_id and generators.
class basic_database
{
public:
    /// Construct an empty database.
    basic_database() = default;
    basic_database(const basic_database&) = delete;
    basic_database& operator=(const basic_database&) = delete;
    /// Destroy the database, invalidating its identifiers and views.
    virtual ~basic_database() = default;

    /// Copy text under a supplied hash, detecting duplicates and collisions.
    /// The caller is responsible for supplying the correct hash of text.
    virtual insert_status insert(hash_type value, std::string_view text) = 0;

    /// Return immutable stored text, or nullopt for an unknown hash/disabled storage.
    [[nodiscard]] virtual std::optional<std::string_view> lookup(hash_type value) const = 0;

    /// Store prefix text plus suffix; throw std::logic_error if prefix is unknown.
    /// The supplied value must be the hash of the complete concatenation.
    virtual insert_status insert_prefix(hash_type value, hash_type prefix, std::string_view suffix)
    {
        const auto text = lookup(prefix);

        if (!text) {
            throw std::logic_error("strnghsh: prefix is not stored");
        }

        return insert(value, std::string(*text).append(suffix));
    }
};

/// An immutable, owning name table captured by database::snapshot() or
/// read_mostly_database::snapshot(). Independent of the source database's lifetime
/// and subsequent insertions. Copies share ownership without copying entries or text.
/// Views remain valid until this snapshot is destroyed, assigned to, or moved from.
/// @par Thread safety
/// After synchronized publication, concurrent lookup() calls need no locks. Lookup uses
/// only const container operations and does not allocate or access the source database.
/// Synchronize assignment, moving from, and destruction against all readers and views.
class database_snapshot
{
public:
    /// Return captured text, or nullopt if the hash was absent when captured.
    /// Acquires no mutex; returned views borrow this snapshot's immutable strings.
    [[nodiscard]] std::optional<std::string_view> lookup(hash_type value) const
    {
        if (!strings_) {
            return std::nullopt;
        }

        const auto entry = strings_->find(value);

        if (entry == strings_->end()) {
            return std::nullopt;
        }

        return *entry->second;
    }

private:
    friend class database;
    friend class read_mostly_database;
    using index = std::unordered_map<hash_type, std::shared_ptr<const std::string>>;

    explicit database_snapshot(std::shared_ptr<const index> strings) : strings_(std::move(strings)) {}

    explicit database_snapshot(const std::unordered_map<hash_type, std::string>& strings)
    {
        auto copy = std::make_shared<index>();
        copy->reserve(strings.size());

        for (const auto& [value, text] : strings) {
            copy->emplace(value, std::make_shared<const std::string>(text));
        }

        strings_ = std::move(copy);
    }

    std::shared_ptr<const index> strings_;
};

/// Thread-safe storage using std::unordered_map; views survive insertions and rehashes.
/// Destruction requires that no threads are accessing the database or its views.
/// @par Thread safety
/// One mutex serializes insertions, duplicate/collision checks, and lookups. Returned text
/// is immutable and can be read without locking while this database lives. Prefix insertion
/// is safe because prefix text never changes, although lookup and insertion lock separately.
/// This mutex does not protect caller-owned input, output variables, or application data.
class database final : public basic_database
{
public:
    /// Insert a pair atomically. Allocation failures propagate; existing text is preserved.
    insert_status insert(hash_type value, std::string_view text) override
    {
        const std::lock_guard lock(mutex_);
        const auto[entry, inserted] = strings_.try_emplace(value, text);
        return inserted ? insert_status::new_string
               : entry->second == text ? insert_status::old_string : insert_status::collision;
    }

    /// Look up a pair under the database mutex; the returned immutable view needs no lock.
    [[nodiscard]] std::optional<std::string_view> lookup(hash_type value) const override
    {
        const std::lock_guard lock(mutex_);
        const auto entry = strings_.find(value);

        if (entry == strings_.end()) {
            return std::nullopt;
        }

        return entry->second;
    }

    /// Copy all current entries under the database mutex for subsequent lookup without locks.
    /// Takes linear time in the stored entries and text size, allocating independent storage.
    /// Allocation/copy failures propagate and leave this database unchanged. The snapshot
    /// may outlive this database; later insertions are visible only in later snapshots.
    [[nodiscard]] database_snapshot snapshot() const
    {
        const std::lock_guard lock(mutex_);
        return database_snapshot(strings_);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<hash_type, std::string> strings_;
};

/// Copy-on-write storage for infrequent or batched registration and frequent lookup.
/// Published indexes and string objects are immutable; updates copy only index entries.
/// Text views remain valid while this database lives, across all subsequent publications.
/// Destruction requires that no calls are in flight; captured snapshots may outlive it.
/// @par Thread safety
/// Writers serialize with one mutex. lookup() and snapshot() acquire an atomic shared_ptr,
/// never the writer mutex. Atomic shared_ptr operations may internally lock; this is not
/// a lock-free guarantee. Retained snapshots perform lookup without synchronization.
/// Publishing new entries costs O(N + K) expected index work for N existing and K batch
/// entries, plus copying new text. Releasing the last owner of an old index reclaims it.
class read_mostly_database final : public basic_database
{
public:
    /// Construct an empty published table. Allocation failures propagate.
    read_mostly_database() : current_(std::make_shared<const index>()) {}

    /// Register one supplied hash/text pair; duplicates and collisions do not publish.
    /// The hash must correspond to text. Allocation failures leave the table unchanged.
    insert_status insert(hash_type value, std::string_view text) override
    {
        const std::pair<hash_type, std::string_view> entry{value, text};
        return insert_batch(std::span(&entry, 1));
    }

    /// Hash and register a batch, cloning the index at most once and publishing atomically.
    /// Return new_string if any entry is new, old_string for an empty/duplicate-only batch,
    /// or collision if any hash conflicts with existing text or earlier batch text.
    /// Collision or allocation failure publishes nothing, including earlier batch entries.
    /// Exceptions propagate. Input views must stay valid and unchanged throughout the call.
    insert_status insert_batch(std::span<const std::string_view> texts)
    {
        return insert_batch_impl(texts, [](std::string_view text) {
            return std::pair{hash(text), text};
        });
    }

    /// Register supplied hash/text pairs with the same atomic batch semantics.
    /// Callers must supply correct hashes; conflicting text rejects the entire batch.
    /// Neither input pairs nor their text are retained; new text is copied before return.
    insert_status insert_batch(std::span<const std::pair<hash_type, std::string_view>> entries)
    {
        return insert_batch_impl(entries, [](const auto & entry) {
            return entry;
        });
    }

    /// Look up the current published table without acquiring the writer mutex.
    /// Return nullopt for an unknown hash. Views survive replacement of the current index
    /// because every later index retains the same immutable string objects.
    [[nodiscard]] std::optional<std::string_view> lookup(hash_type value) const override
    {
        return snapshot().lookup(value);
    }

    /// Acquire the current owning snapshot without copying entries or text.
    /// Atomic acquisition may internally lock; subsequent snapshot lookups do not.
    /// Snapshot acquisition does not allocate. Later batches are visible only to later
    /// acquisitions. A captured snapshot and its text can outlive this database.
    [[nodiscard]] database_snapshot snapshot() const
    {
        return database_snapshot(current_.load(std::memory_order_acquire));
    }

private:
    using index = database_snapshot::index;

    template<class Entries, class GetEntry>
    insert_status insert_batch_impl(const Entries& entries, GetEntry get_entry)
    {
        const std::lock_guard lock(writer_mutex_);
        const auto previous = current_.load(std::memory_order_relaxed);
        std::shared_ptr<index> next;

        for (const auto& input : entries) {
            const auto[value, text] = get_entry(input);
            const auto& active = next ? *next : *previous;
            const auto found = active.find(value);

            if (found != active.end()) {
                if (*found->second != text) {
                    return insert_status::collision;
                }

                continue;
            }

            if (!next) {
                next = std::make_shared<index>(*previous);
            }

            next->emplace(value, std::make_shared<const std::string>(text));
        }

        if (!next) {
            return insert_status::old_string;
        }

        current_.store(std::move(next), std::memory_order_release);
        return insert_status::new_string;
    }

    std::mutex writer_mutex_;
    std::atomic<std::shared_ptr<const index>> current_;
};

/// Return the shared default database, owned by the library with static storage duration.
/// Initialization and concurrent insertion/lookup are thread-safe. Initialization failures
/// propagate and a later call retries. Entries remain until destruction at program shutdown;
/// identifiers and returned views must not be used after the database is destroyed.
[[nodiscard]] database& default_database()
{
    static database instance;
    return instance;
}

/// Stateless, thread-safe storage opt-out; no text recovery or uniqueness/collision checks.
class dummy_database final : public basic_database
{
public:
    /// Accept a pair without storing it.
    insert_status insert(hash_type, std::string_view) override
    {
        return insert_status::new_string;
    }

    /// Accept a prefixed pair without requiring stored prefix text.
    insert_status insert_prefix(hash_type, hash_type, std::string_view) override
    {
        return insert_status::new_string;
    }

    /// Storage is disabled, so always return nullopt.
    [[nodiscard]] std::optional<std::string_view> lookup(hash_type) const override
    {
        return std::nullopt;
    }
};

/// Thrown when different strings share a hash; the existing entry remains intact.
class collision_error : public std::runtime_error
{
public:
    /// Construct a collision diagnostic.
    collision_error() : std::runtime_error("strnghsh: hash collision") {}
};

/// A hash plus a non-owning database pointer. Copying does not access the database.
/// The database must outlive this object; equality includes database identity.
/// @par Thread safety
/// After synchronized publication, concurrent reads, comparisons, and copies are safe.
/// Synchronize assignment to an object being accessed by other threads. Construction and
/// string() require a thread-safe database or external synchronization. Input text must
/// stay valid and unchanged during construction; use a separate insert_status per call.
class string_id
{
public:
    /// Hash and store text in default_database(); prefix composition retains that database.
    /// Throw collision_error on a collision; initialization/allocation exceptions propagate.
    explicit string_id(std::string_view text) : string_id(text, default_database()) {}

    /// Hash and store text; throw collision_error on a collision.
    string_id(std::string_view text, basic_database& db) : id_(hash(text)), db_(&db)
    {
        check(db_->insert(id_, text));
    }

    /// Hash and store text, reporting collisions without throwing collision_error.
    /// Allocation and database exceptions still propagate. On collision, string() is the old text.
    string_id(std::string_view text, basic_database& db, insert_status& status)
        : id_(hash(text)), db_(&db)
    {
        status = db_->insert(id_, text);
    }

    /// Append suffix to a prefix in its database; throw collision_error on collision.
    string_id(const string_id& prefix, std::string_view suffix)
        : id_(hash(suffix, prefix.id_)), db_(prefix.db_)
    {
        check(db_->insert_prefix(id_, prefix.id_, suffix));
    }

    /// Append suffix, reporting collisions through status; other exceptions propagate.
    string_id(const string_id& prefix, std::string_view suffix, insert_status& status)
        : id_(hash(suffix, prefix.id_)), db_(prefix.db_)
    {
        status = db_->insert_prefix(id_, prefix.id_, suffix);
    }

    /// Return the hash, suitable for switch labels and raw-hash comparisons.
    [[nodiscard]] hash_type hash_code() const noexcept
    {
        return id_;
    }

    /// Return the database that owns this identifier's text.
    [[nodiscard]] basic_database& database() const noexcept
    {
        return *db_;
    }

    /// Retrieve stored text, or nullopt when storage is disabled.
    /// With strnghsh::database the returned view can be read concurrently, without locking,
    /// until the database is destroyed; this call itself acquires the database mutex.
    [[nodiscard]] std::optional<std::string_view> string() const
    {
        return db_->lookup(id_);
    }

    /// Compare hash and database identity; C++20 supplies operator!=.
    bool operator==(const string_id&) const = default;

    /// Compare only the hash; C++20 supplies reversed comparisons and operator!=.
    friend bool operator==(const string_id& id, hash_type value) noexcept
    {
        return id.id_ == value;
    }

private:
    static void check(insert_status status)
    {
        if (status == insert_status::collision) {
            throw collision_error();
        }
    }

    hash_type id_;
    basic_database* db_;
};

/// Thrown after a generator's bounded attempts fail to produce a new identifier.
class generation_error : public std::runtime_error
{
public:
    /// Construct a generation diagnostic.
    generation_error() : std::runtime_error("strnghsh: unique identifier generation exhausted") {}
};

}

namespace strnghsh::detail
{
template<class Generator>
string_id generate(const string_id& prefix, Generator&& suffix)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        insert_status status;
        string_id result(prefix, suffix(), status);

        if (status == insert_status::new_string) {
            return result;
        }
    }

    throw generation_error();
}
}

export namespace strnghsh
{

/// Thread-safe numeric suffix generator; database access must also be thread-safe.
/// Duplicates/collisions retry at most eight times before throwing generation_error.
/// With dummy_database, uniqueness cannot be checked. Counter arithmetic wraps modulo 2^64.
/// @par Thread safety
/// Concurrent operator()() and discard() calls are safe after synchronized publication.
/// The generator and its database must outlive all calls. Atomic counter allocation does
/// not order completion or synchronize unrelated data. Retries and discard() can skip
/// values; fixed-width truncation and wrap can repeat suffixes.
class counter_generator
{
public:
    /// Start at counter; nonzero width pads with zeros or keeps the last width digits.
    explicit counter_generator(string_id prefix, hash_type counter = 0, std::size_t width = 0)
        : prefix_(prefix), counter_(counter), width_(width) {}

    /// Generate a fresh identifier. Allocation/database exceptions propagate.
    [[nodiscard]] string_id operator()()
    {
        return detail::generate(prefix_, [this] {
            auto text = std::to_string(counter_.fetch_add(1, std::memory_order_relaxed));

            if (width_ > text.size())
            {
                text.insert(0, width_ - text.size(), '0');
            } else if (width_ != 0 && width_ < text.size())
            {
                text.erase(0, text.size() - width_);
            }

            return text;
        });
    }

    /// Advance the counter by count, modulo 2^64.
    void discard(hash_type count) noexcept
    {
        counter_.fetch_add(count, std::memory_order_relaxed);
    }

private:
    string_id prefix_;
    std::atomic<hash_type> counter_;
    std::size_t width_;
};

/// Random suffix generator using a standard uniform random bit generator.
/// Not thread-safe. Tries eight times before throwing generation_error.
/// With dummy_database, uniqueness cannot be checked.
/// @par Thread safety
/// Use one generator per thread with a shared synchronized database, or guard every
/// operator()() and discard() call on a shared generator with the same external mutex.
/// The database mutex does not protect the engine. Distinct seeds do not guarantee
/// distinct names; database insertion checks uniqueness across workers.
template<class Engine, std::size_t Length>
class random_generator
{
public:
    /// Own the engine and alphabet; throw std::invalid_argument for an empty alphabet.
    explicit random_generator(string_id prefix, Engine engine = Engine{},
                              std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
        : prefix_(prefix), engine_(std::move(engine)), alphabet_(alphabet)
    {
        if (alphabet_.empty()) {
            throw std::invalid_argument("strnghsh: empty alphabet");
        }
    }

    /// Generate a fresh identifier. Allocation/database/engine exceptions propagate.
    [[nodiscard]] string_id operator()()
    {
        return detail::generate(prefix_, [this] {
            std::uniform_int_distribution<std::size_t> distribution(0, alphabet_.size() - 1);
            std::string text(Length, '\0');

            for (auto& character : text)
            {
                character = alphabet_[distribution(engine_)];
            }

            return text;
        });
    }

    /// Discard engine states (not identifiers), forwarding to Engine::discard.
    void discard(unsigned long long count)
    {
        engine_.discard(count);
    }

private:
    string_id prefix_;
    Engine engine_;
    std::string alphabet_;
};

}

/// Standard unordered-container support for strnghsh identifiers.
template<>
struct std::hash<strnghsh::string_id> {
    /// Hash the numeric identifier; equality additionally checks database identity.
    std::size_t operator()(const strnghsh::string_id& id) const noexcept
    {
        return std::hash<strnghsh::hash_type> {}(id.hash_code());
    }
};
