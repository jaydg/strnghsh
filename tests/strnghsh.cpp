// Distributed under the zlib license; see LICENSE.
import <array>;
import <atomic>;
import <cstdlib>;
import <iostream>;
import <limits>;
import <optional>;
import <random>;
import <stdexcept>;
import <string>;
import <string_view>;
import <thread>;
import <type_traits>;
import <unordered_set>;
import <vector>;

import strnghsh;

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

void collisions()
{
    sh::database db;
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

void generators()
{
    sh::database db;
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

void concurrent_access()
{
    sh::database db;
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
        identifiers();
        collisions();
        disabled_storage();
        generators();
        concurrent_access();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "All strnghsh tests passed\n";
}
