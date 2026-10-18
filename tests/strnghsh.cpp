// Distributed under the zlib license; see LICENSE.
import std;

import strnghsh;

namespace
{
// Disabled outside the allocation-failure test; worker threads have their own budget.
thread_local int allocation_budget = -1;
}

void* operator new (std::size_t size)
{
    if (allocation_budget == 0) {
        throw std::bad_alloc();
    }

    if (allocation_budget > 0) {
        --allocation_budget;
    }

    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }

    throw std::bad_alloc();
}

void operator delete (void* memory) noexcept
{
    std::free(memory);
}

void operator delete (void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace sh = strnghsh;
using namespace sh::literals;
using namespace std::string_view_literals;

static_assert(sh::hash("") == 0xcbf29ce484222325ULL);
static_assert("a"_id == 0xaf63dc4c8601ec8cULL);
static_assert("foobar"_id == 0x85944171f73967e8ULL);
static_assert(sh::hash("bar", "foo"_id) == "foobar"_id);
static_assert("a\0b"_id == sh::hash("a\0b"sv));
static_assert("a\0b"_id != "a"_id);
static_assert(sh::hash("\xff"sv) == 0xaf64724c8602eb6eULL);
static_assert(std::is_trivially_copyable_v<sh::string_id>);
static_assert(!std::is_move_constructible_v<sh::database>);
static_assert(!std::is_move_constructible_v<sh::read_mostly_database>);
static_assert(!std::is_convertible_v<std::string_view, sh::string_id>);

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template<class Exception, class Function>
void throws(Function&& function)
{
    try {
        function();
    } catch (const Exception&) {
        return;
    }

    throw std::runtime_error("expected exception was not thrown");
}

void default_database_access()
{
    // Run first so workers exercise concurrent first use of the shared table.
    std::array<std::optional<sh::string_id>, 4> ids;
    std::atomic<bool> success{true};
    {
        std::vector<std::jthread> threads;

        for (auto& id : ids) {
            threads.emplace_back([&id, &success] {
                try
                {
                    id.emplace("default-shared");
                } catch (...)
                {
                    success = false;
                }
            });
        }
    }
    require(success, "concurrent default database construction");

    for (const auto& id : ids) {
        require(id.has_value() && *id == *ids.front(), "default database identity across workers");
        require(&id->database() == &sh::default_database(), "default database accessor");
    }

    sh::string_id shared("default-shared", sh::default_database());
    require(shared == *ids.front(), "explicit and implicit default database match");
    sh::database local;
    require(shared != sh::string_id("default-shared", local), "default and local databases differ");
    sh::string_id composed(shared, "/child");
    require(composed == sh::string_id("default-shared/child"), "default prefix database");
    sh::counter_generator generator(composed);
    const auto generated = generator();
    require(generated == sh::string_id("default-shared/child0"), "default generator database");
    const sh::hash_type compact = generated.hash_code();
    require(sh::default_database().lookup(compact) == "default-shared/child0"sv,
            "recover default text from bare hash");

    sh::default_database().insert("default-collision"_id, "conflicting text");
    throws<sh::collision_error>([] { sh::string_id id("default-collision"); });
}

void identifiers()
{
    sh::database db, other;
    sh::insert_status status;
    sh::string_id a("hello", db, status);
    require(status == sh::insert_status::new_string, "first insertion");
    sh::string_id b("hello", db, status);
    require(status == sh::insert_status::old_string, "duplicate insertion");
    require(a == b && a == "hello"_id && "hello"_id == a, "identifier equality");
    require(a != "world"_id && "world"_id != a, "raw hash inequality");
    require(a != sh::string_id("hello", other), "database identity");
    require(&a.database() == &db, "database accessor");
    require(a.string() == "hello"sv, "text recovery");
    require(!db.lookup("missing"_id), "missing lookup");
    require(sh::string_id("", db).string() == ""sv, "empty text is present");

    switch (a.hash_code()) {
    case "hello"_id:
        break;

    default:
        throw std::runtime_error("switch label");
    }

    std::unordered_set<sh::string_id> ids{a, b, sh::string_id("world", db)};
    require(ids.size() == 2, "std::hash support");

    std::string source = "owned";
    sh::string_id owned(source, db);
    source.assign("changed");
    require(owned.string() == "owned"sv, "database owns text");

    const std::array text{'x', 'y', 'z'};
    sh::string_id bounded(std::string_view(text.data(), 2), db);
    require(bounded == "xy"_id && bounded.string() == "xy"sv, "bounded nonterminated text");
    sh::string_id binary("a\0b"sv, db);
    sh::string_id combined(binary, "\0c"sv);
    require(combined == "a\0b\0c"_id && combined.string() == "a\0b\0c"sv, "binary prefix");
    require(sh::string_id(a, " world") == sh::string_id("hello world", db), "prefix composition");

    const auto view = *a.string();

    for (int i = 0; i < 10000; ++i) {
        sh::string_id inserted(std::to_string(i), db);
    }

    require(view == "hello" && view.data() == a.string()->data(), "views survive rehash");
}

template<class Database = sh::database>
void collisions()
{
    Database db;
    // Inject a conflicting pair to exercise the collision path deterministically.
    require(db.insert("second"_id, "first") == sh::insert_status::new_string, "seed collision");
    require(db.insert("second"_id, "second") == sh::insert_status::collision, "collision status");
    throws<sh::collision_error>([&] { sh::string_id id("second", db); });
    sh::insert_status status;
    sh::string_id id("second", db, status);
    require(status == sh::insert_status::collision && id.string() == "first"sv,
            "collision preserves old text");

    sh::string_id prefix("prefix", db);
    db.insert("prefix-suffix"_id, "conflict");
    throws<sh::collision_error>([&] { sh::string_id combined(prefix, "-suffix"); });
    sh::string_id combined(prefix, "-suffix", status);
    require(status == sh::insert_status::collision, "prefix collision status");
    throws<std::logic_error>([&] { db.insert_prefix(0, "unknown"_id, "suffix"); });
}

void disabled_storage()
{
    sh::dummy_database db;
    sh::string_id prefix("foo", db);
    sh::string_id combined(prefix, "bar");
    require(combined == "foobar"_id && !combined.string(), "disabled prefix storage");
    sh::counter_generator generator(prefix);
    require(generator() == "foo0"_id, "disabled storage generation");
}

template<class Database = sh::database>
void generators()
{
    Database db;
    sh::string_id prefix("entity-", db);
    sh::counter_generator counter(prefix, 0, 4);
    require(counter().string() == "entity-0000"sv, "zero padding");
    counter.discard(8);
    require(counter().string() == "entity-0009"sv, "counter discard");
    sh::counter_generator truncated(prefix, 1234, 2);
    require(truncated().string() == "entity-34"sv, "counter truncation");
    sh::counter_generator wrap(prefix, std::numeric_limits<sh::hash_type>::max());
    require(wrap().string() == "entity-18446744073709551615"sv, "maximum counter");
    require(wrap().string() == "entity-0"sv, "counter wrap");

    sh::string_id retry_prefix("retry-", db);
    sh::string_id existing(retry_prefix, "0");
    sh::counter_generator retry(retry_prefix);
    require(retry().string() == "retry-1"sv, "skip existing suffix");

    sh::string_id exhausted_prefix("full-", db);

    for (int i = 0; i < 8; ++i) {
        sh::string_id entry(exhausted_prefix, std::to_string(i));
    }

    sh::counter_generator exhausted(exhausted_prefix);
    throws<sh::generation_error>([&] { (void)exhausted(); });
    require(exhausted().string() == "full-8"sv, "eight bounded generation attempts");

    sh::random_generator<std::mt19937, 12> random(prefix, std::mt19937{42}, "abc");
    random.discard(10);
    std::unordered_set<sh::string_id> ids;

    for (int i = 0; i < 100; ++i) {
        auto id = random();
        const auto text = *id.string();
        require(text.starts_with("entity-") && text.size() == 19, "random prefix and length");
        require(text.substr(7).find_first_not_of("abc") == std::string_view::npos, "random alphabet");
        require(ids.insert(id).second, "random uniqueness");
    }

    sh::random_generator<std::mt19937, 1> single(prefix, std::mt19937{42}, "x");
    require(single().string() == "entity-x"sv, "single-character alphabet");
    throws<sh::generation_error>([&] { (void)single(); });
    throws<std::invalid_argument>([&] {
        sh::random_generator<std::mt19937, 1> invalid(prefix, std::mt19937{}, "");
    });
    sh::random_generator<std::mt19937, 0> empty(prefix);
    throws<sh::generation_error>([&] { (void)empty(); });
}

template<class Database = sh::database>
void snapshots()
{
    Database db;
    const auto empty = db.snapshot();
    require(!empty.lookup("missing"_id), "empty snapshot lookup");
    sh::string_id original("snapshot-name", db);
    sh::string_id blank("", db);
    sh::string_id binary("a\0b"sv, db);
    const auto captured = db.snapshot();
    const auto view = captured.lookup(original.hash_code());
    require(view == "snapshot-name"sv, "snapshot text recovery");
    require(captured.lookup(blank.hash_code()) == ""sv, "snapshot empty string");
    require(captured.lookup(binary.hash_code()) == "a\0b"sv, "snapshot binary string");
    require(!empty.lookup(original.hash_code()), "snapshot retains empty state");
    require(db.insert(original.hash_code(), "conflict") == sh::insert_status::collision,
            "snapshot does not change collision detection");

    for (int i = 0; i < 1000; ++i) {
        sh::string_id added("later-" + std::to_string(i), db);
    }

    require(!captured.lookup("later-999"_id), "snapshot excludes later insertions");
    require(db.snapshot().lookup("later-999"_id) == "later-999"sv, "fresh snapshot includes insertions");
    require(view == "snapshot-name"sv, "snapshot view survives source rehash");

    const auto independent = [] {
        Database temporary;
        sh::string_id id("text owned independently of the source database", temporary);
        return temporary.snapshot();
    }();
    require(independent.lookup("text owned independently of the source database"_id)
            == "text owned independently of the source database"sv, "snapshot outlives source");
    const auto copy = independent;
    require(copy.lookup("text owned independently of the source database"_id)
            == independent.lookup("text owned independently of the source database"_id),
            "snapshot copy retains text");
}

void concurrent_snapshot_access()
{
    sh::database db;
    sh::string_id original("snapshot-shared", db);
    const auto captured = db.snapshot();
    std::atomic<bool> success{true};
    {
        std::vector<std::jthread> readers;

        for (int worker = 0; worker < 4; ++worker) {
            readers.emplace_back([&captured, &success] {
                for (int i = 0; i < 5000; ++i)
                {
                    if (captured.lookup("snapshot-shared"_id) != "snapshot-shared"sv
                            || captured.lookup("snapshot-added-0"_id)) {
                        success = false;
                    }
                }
            });
        }

        for (int i = 0; i < 500; ++i) {
            const auto text = "snapshot-added-" + std::to_string(i);
            sh::string_id added(text, db);

            if (i % 50 == 0) {
                const auto fresh = db.snapshot();
                require(fresh.lookup(added.hash_code()) == text, "capture during concurrent reads");
            }
        }
    }
    require(success, "concurrent snapshot readers and source insertions");
    require(!captured.lookup("snapshot-added-0"_id), "shared snapshot remains unchanged");
}

void read_mostly_batches()
{
    sh::read_mostly_database db;
    require(db.insert_batch(std::span<const std::string_view> {}) == sh::insert_status::old_string,
            "empty batch");
    const std::array initial{""sv, "a\0b"sv, "read-mostly"sv, "read-mostly"sv};
    require(db.insert_batch(initial) == sh::insert_status::new_string, "register batch with duplicates");
    require(db.insert_batch(initial) == sh::insert_status::old_string, "duplicate-only batch");
    require(db.lookup(""_id) == ""sv && db.lookup("a\0b"_id) == "a\0b"sv, "batch binary and empty text");
    require(!db.lookup("absent"_id), "read-mostly missing lookup");
    sh::string_id id("read-mostly", db);
    const auto view = *id.string();
    const auto before = db.snapshot();
    std::string source = "a long string owned by the read-mostly database";
    const std::array addition{std::string_view(source), "batch-new"sv};
    require(db.insert_batch(addition) == sh::insert_status::new_string, "publish additional batch");
    source.assign("changed");
    require(db.lookup("a long string owned by the read-mostly database"_id)
            == "a long string owned by the read-mostly database"sv, "batch owns input text");
    require(!before.lookup("batch-new"_id) && db.lookup("batch-new"_id) == "batch-new"sv,
            "old snapshot remains frozen while lookup refreshes");
    require(view.data() == db.lookup(id.hash_code())->data(), "publications preserve string addresses");
    require(view.data() == before.lookup(id.hash_code())->data(), "snapshot shares existing strings");

    using entry = std::pair<sh::hash_type, std::string_view>;
    const std::array conflicting{entry{"rollback"_id, "rollback"}, entry{id.hash_code(), "conflict"}};
    require(db.insert_batch(conflicting) == sh::insert_status::collision, "batch collision with existing text");
    require(!db.lookup("rollback"_id) && id.string() == "read-mostly"sv, "collision rolls back entire batch");
    const std::array internal{entry{"internal"_id, "internal"}, entry{"internal"_id, "different"}};
    require(db.insert_batch(internal) == sh::insert_status::collision, "collision inside batch");
    require(!db.lookup("internal"_id), "internal collision publishes nothing");
    const std::array duplicate{entry{"internal"_id, "internal"}, entry{"internal"_id, "internal"}};
    require(db.insert_batch(duplicate) == sh::insert_status::new_string, "duplicates inside paired batch");
    require(db.insert_batch(duplicate) == sh::insert_status::old_string, "paired batch duplicate status");
    const auto copy = before;
    require(copy.lookup(id.hash_code())->data() == view.data(), "snapshot copies share immutable storage");
}

void read_mostly_allocation_failures()
{
    sh::read_mostly_database db;
    sh::string_id seed("allocation-seed", db);
    const auto before = db.snapshot();
    const auto view = *seed.string();
    const std::array names{"allocation-first"sv, "a long second entry requiring its own text allocation"sv};
    bool completed = false;
    int failures = 0;

    for (int budget = 0; budget < 128; ++budget) {
        bool failed = false;
        sh::insert_status status = sh::insert_status::old_string;
        {
            struct fail_allocations {
                explicit fail_allocations(int count)
                {
                    allocation_budget = count;
                }
                ~fail_allocations()
                {
                    allocation_budget = -1;
                }
            } guard(budget);

            if (budget == 0) {
                const auto captured = db.snapshot();
                require(captured.lookup(seed.hash_code()) == "allocation-seed"sv,
                        "snapshot acquisition and lookup do not allocate");
                require(db.lookup(seed.hash_code()) == "allocation-seed"sv,
                        "live lookup does not allocate");
                require(db.insert(seed.hash_code(), "allocation-seed") == sh::insert_status::old_string,
                        "duplicate registration does not allocate");
            }

            try {
                status = db.insert_batch(names);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }

        if (failed) {
            ++failures;
            require(!db.lookup(sh::hash(names[0])) && !db.lookup(sh::hash(names[1])),
                    "allocation failure publishes no part of batch");
            require(seed.string() == "allocation-seed"sv && seed.string()->data() == view.data(),
                    "allocation failure preserves existing text and views");
        } else {
            require(status == sh::insert_status::new_string, "batch succeeds after allocation failures");
            completed = true;
            break;
        }
    }

    require(completed && failures > 0, "exercise batch allocation failures through successful retry");
    require(db.lookup(sh::hash(names[0])) == names[0] && db.lookup(sh::hash(names[1])) == names[1],
            "successful retry publishes full batch");
    require(!before.lookup(sh::hash(names[0])), "failed and successful batches preserve held snapshots");
}

void concurrent_read_mostly_access()
{
    sh::read_mostly_database db;
    sh::string_id seed("concurrent-seed", db);
    const auto held = db.snapshot();
    const auto view = *seed.string();
    std::atomic<bool> success{true};
    {
        std::vector<std::jthread> threads;

        for (int writer = 0; writer < 2; ++writer) {
            threads.emplace_back([&db, &success, writer] {
                try
                {
                    for (int batch = 0; batch < 32; ++batch) {
                        const auto prefix = std::to_string(writer) + "/" + std::to_string(batch);
                        const std::string first = prefix + "/first", last = prefix + "/last";
                        const std::array names{std::string_view(first), std::string_view(last), "concurrent-seed"sv};

                        if (db.insert_batch(names) != sh::insert_status::new_string) {
                            success = false;
                        }
                    }
                } catch (...)
                {
                    success = false;
                }
            });
        }

        for (int reader = 0; reader < 2; ++reader) {
            threads.emplace_back([&db, &held, &success] {
                try
                {
                    for (int pass = 0; pass < 100; ++pass) {
                        const auto captured = db.snapshot();

                        for (int writer = 0; writer < 2; ++writer) {
                            for (int batch = 0; batch < 32; ++batch) {
                                const auto prefix = std::to_string(writer) + "/" + std::to_string(batch);
                                const auto first = captured.lookup(sh::hash(prefix + "/first"));
                                const auto last = captured.lookup(sh::hash(prefix + "/last"));

                                if (first.has_value() != last.has_value()
                                        || (first && *first != prefix + "/first")
                                        || (last && *last != prefix + "/last")) {
                                    success = false;
                                }
                            }
                        }

                        if (db.lookup("concurrent-seed"_id) != "concurrent-seed"sv
                                || held.lookup("0/0/first"_id)) {
                            success = false;
                        }
                    }
                } catch (...)
                {
                    success = false;
                }
            });
        }
    }
    require(success, "concurrent batch publication and snapshot readers");

    for (int writer = 0; writer < 2; ++writer) {
        for (int batch = 0; batch < 32; ++batch) {
            const auto prefix = std::to_string(writer) + "/" + std::to_string(batch);
            require(db.lookup(sh::hash(prefix + "/first")) == prefix + "/first"
                    && db.lookup(sh::hash(prefix + "/last")) == prefix + "/last", "writers lose no updates");
        }
    }

    require(view == "concurrent-seed"sv && view.data() == db.lookup(seed.hash_code())->data(),
            "borrowed views survive concurrent index replacement");
}

template<class Database = sh::database>
void concurrent_access()
{
    Database db;
    sh::string_id prefix("thread-", db);
    sh::counter_generator generator(prefix);
    std::atomic<bool> success{true};
    std::array<std::vector<sh::hash_type>, 4> generated;
    {
        std::vector<std::jthread> threads;

        for (auto& values : generated) {
            threads.emplace_back([&db, &generator, &values, &success] {
                try
                {
                    for (int i = 0; i < 500; ++i) {
                        sh::string_id shared("shared", db);

                        if (shared.string() != "shared"sv) {
                            success = false;
                        }

                        auto id = generator();

                        if (sh::hash(*id.string()) != id.hash_code()) {
                            success = false;
                        }

                        values.push_back(id.hash_code());
                    }
                } catch (...)
                {
                    success = false;
                }
            });
        }
    }
    require(success, "concurrent insertion and lookup");
    std::unordered_set<sh::hash_type> unique;

    for (const auto& values : generated) {
        unique.insert(values.begin(), values.end());
    }

    require(unique.size() == 2000, "concurrent counter uniqueness");
}
}

int main()
{
    try {
        default_database_access();
        identifiers();
        collisions();
        disabled_storage();
        generators();
        snapshots();
        concurrent_snapshot_access();
        concurrent_access();
        collisions<sh::read_mostly_database>();
        generators<sh::read_mostly_database>();
        snapshots<sh::read_mostly_database>();
        read_mostly_batches();
        read_mostly_allocation_failures();
        concurrent_read_mostly_access();
        concurrent_access<sh::read_mostly_database>();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "All strnghsh tests passed\n";
}
