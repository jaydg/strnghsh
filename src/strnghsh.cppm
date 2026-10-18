// Altered C++20 reimplementation of foonathan/string_id.
// Copyright (C) 2014-2015 Jonathan Müller <jonathanmueller.dev@gmail.com>
// Distributed under the zlib license; see LICENSE.
module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

export module strnghsh;

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

/// Thread-safe storage using std::unordered_map; views survive insertions and rehashes.
/// Destruction requires that no threads are accessing the database or its views.
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

private:
    mutable std::mutex mutex_;
    std::unordered_map<hash_type, std::string> strings_;
};

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
class string_id
{
public:
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
