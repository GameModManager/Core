#include "ui/controllers/mod_list_controller.h"

#include <QString>

#include <catch2/catch_test_macros.hpp>

using ui::mod_list_text_matches;

TEST_CASE("mod_list_text_matches: empty query matches everything", "[ui][filter]") {
  CHECK(mod_list_text_matches(QString(), QStringLiteral("Foo"),
                              QStringLiteral("foo_id")));
  CHECK(mod_list_text_matches(QStringLiteral("   "), QStringLiteral("Foo"),
                              QStringLiteral("foo_id")));
}

TEST_CASE("mod_list_text_matches: plain substring on name or id", "[ui][filter]") {
  CHECK(mod_list_text_matches(QStringLiteral("foo"), QStringLiteral("FooBar"),
                              QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("foo"), QStringLiteral("x"),
                              QStringLiteral("foo_id")));
  CHECK_FALSE(mod_list_text_matches(QStringLiteral("zzz"), QStringLiteral("FooBar"),
                                    QStringLiteral("foo_id")));
}

TEST_CASE("mod_list_text_matches: AND via space", "[ui][filter]") {
  CHECK(mod_list_text_matches(QStringLiteral("foo bar"), QStringLiteral("FooBar"),
                              QStringLiteral("x")));
  CHECK_FALSE(mod_list_text_matches(QStringLiteral("foo bar"), QStringLiteral("Foo"),
                                    QStringLiteral("x")));
  CHECK_FALSE(mod_list_text_matches(QStringLiteral("foo bar"), QStringLiteral("Bar"),
                                    QStringLiteral("x")));
}

TEST_CASE("mod_list_text_matches: OR via semicolon/pipe/OR", "[ui][filter]") {
  CHECK(mod_list_text_matches(QStringLiteral("foo;bar"), QStringLiteral("Foo"),
                              QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("foo;bar"), QStringLiteral("Bar"),
                              QStringLiteral("x")));
  CHECK_FALSE(mod_list_text_matches(QStringLiteral("foo;bar"), QStringLiteral("Baz"),
                                    QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("foo|bar"), QStringLiteral("Bar"),
                              QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("foo OR bar"), QStringLiteral("Bar"),
                              QStringLiteral("x")));
}

TEST_CASE("mod_list_text_matches: wildcard", "[ui][filter]") {
  CHECK(mod_list_text_matches(QStringLiteral("foo*"), QStringLiteral("FooBar"),
                              QStringLiteral("x")));
  CHECK_FALSE(mod_list_text_matches(QStringLiteral("foo*"), QStringLiteral("BarFoo"),
                                    QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("*bar"), QStringLiteral("FooBar"),
                              QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("f?o"), QStringLiteral("Foo"),
                              QStringLiteral("x")));
}

TEST_CASE("mod_list_text_matches: malformed no-op", "[ui][filter]") {
  // operator-only queries match everything
  CHECK(mod_list_text_matches(QStringLiteral(";"), QStringLiteral("Foo"),
                              QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("|"), QStringLiteral("Foo"),
                              QStringLiteral("x")));
  CHECK(mod_list_text_matches(QStringLiteral("OR"), QStringLiteral("Foo"),
                              QStringLiteral("x")));
  // unbalanced quote
  CHECK(mod_list_text_matches(QStringLiteral("\"foo"), QStringLiteral("Foo"),
                              QStringLiteral("x")));
}