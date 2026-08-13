// Wrap-math regression for the Flags column (FlagsDelegate). The delegate paints
// each flag icon individually and, when the column is too narrow for all of
// them, wraps to extra lines and grows the row height. These checks pin the
// pure-int layout helpers in mod_table_view.h so paint() and sizeHint() can
// never disagree about how many icons fit.
#include "ui/widgets/mod_table_view.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("flags delegate", "[ui]") {
    // 16px icons with 2px spacing in the default 80px Flags column.
    REQUIRE(ui::flags_icons_per_line(80, 16, 2) == 4);
    // Exactly one full line.
    REQUIRE(ui::flags_icons_per_line(18, 16, 2) == 1);
    // Column narrower than a single icon still lays out one per line.
    REQUIRE(ui::flags_icons_per_line(10, 16, 2) == 1);
    // Wider column, same rule.
    REQUIRE(ui::flags_icons_per_line(160, 16, 2) == 8);

    REQUIRE(ui::flags_icon_lines(1, 4) == 1);
    REQUIRE(ui::flags_icon_lines(4, 4) == 1);
    REQUIRE(ui::flags_icon_lines(5, 4) == 2);  // wraps to a second line
    REQUIRE(ui::flags_icon_lines(9, 4) == 3);
    REQUIRE(ui::flags_icon_lines(0, 4) == 0);
}
