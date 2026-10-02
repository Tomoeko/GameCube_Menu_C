#include "menu_internal.h"

#include <math.h>

static const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int year, int month) {
    if (month == 2 && leap_year(year))
        return 29;
    return month_days[month - 1];
}

bool gc_date_time_valid(const gc_date_time *clock) {
    if (!clock || clock->year < 2000 || clock->year > 2099 || clock->month < 1 ||
        clock->month > 12 || clock->hour < 0 || clock->hour > 23 || clock->minute < 0 ||
        clock->minute > 59 || clock->second < 0 || clock->second > 59)
        return false;
    return clock->day >= 1 && clock->day <= days_in_month(clock->year, clock->month);
}

static int64_t clock_seconds(const gc_date_time *clock) {
    int64_t days = 0;
    int year;
    int month;

    for (year = 2000; year < clock->year; ++year)
        days += leap_year(year) ? 366 : 365;
    for (month = 1; month < clock->month; ++month)
        days += days_in_month(clock->year, month);
    days += clock->day - 1;
    return days * 86400 + clock->hour * 3600 + clock->minute * 60 + clock->second;
}

static void clock_from_seconds(gc_date_time *clock, int64_t seconds) {
    int64_t days = seconds / 86400;
    int year_days;
    int month_length;

    clock->hour = (int)((seconds % 86400) / 3600);
    clock->minute = (int)((seconds % 3600) / 60);
    clock->second = (int)(seconds % 60);
    clock->year = 2000;
    while (true) {
        year_days = leap_year(clock->year) ? 366 : 365;
        if (days < year_days)
            break;
        days -= year_days;
        ++clock->year;
    }
    clock->month = 1;
    while (true) {
        month_length = days_in_month(clock->year, clock->month);
        if (days < month_length)
            break;
        days -= month_length;
        ++clock->month;
    }
    clock->day = (int)days + 1;
}

unsigned gc_date_time_weekday(const gc_date_time *clock) {
    if (!gc_date_time_valid(clock))
        return 0;
    /* January 1, 2000 is Saturday; Sunday is the first displayed weekday. */
    return (unsigned)((clock_seconds(clock) / 86400 + 6) % 7);
}

bool gc_menu_set_clock(gc_menu *menu, const gc_date_time *clock) {
    if (!menu || !gc_date_time_valid(clock))
        return false;
    menu->clock = *clock;
    menu->clock_fraction = 0.0;
    return true;
}

void gc_menu_advance_clock(gc_menu *menu, double elapsed_seconds) {
    if (!menu || !isfinite(elapsed_seconds) || elapsed_seconds <= 0.0)
        return;
    gc_date_time *clock = menu->page == GC_PAGE_CALENDAR && menu->editing
                              ? &menu->clock_before_edit
                              : &menu->clock;
    menu->clock_fraction += fmod(elapsed_seconds, 3155760000.0);
    double whole_seconds = floor(menu->clock_fraction);
    menu->clock_fraction -= whole_seconds;
    int64_t next_seconds =
        (clock_seconds(clock) + (int64_t)whole_seconds) % 3155760000LL;
    clock_from_seconds(clock, next_seconds);
    if (clock != &menu->clock) {
        if (menu->editor_index < 3) {
            menu->clock.hour = clock->hour;
            menu->clock.minute = clock->minute;
            menu->clock.second = clock->second;
        } else {
            menu->clock.year = clock->year;
            menu->clock.month = clock->month;
            menu->clock.day = clock->day;
        }
    }
}

static int wrap_value(int value, int minimum, int maximum) {
    if (value < minimum)
        return maximum;
    if (value > maximum)
        return minimum;
    return value;
}

static void clamp_calendar_day(gc_date_time *clock) {
    int maximum = days_in_month(clock->year, clock->month);

    if (clock->day > maximum)
        clock->day = maximum;
}

gc_calendar_field gc_menu_calendar_field(const gc_menu *menu) {
    if (!menu || menu->editor_index > 5)
        return GC_CALENDAR_DAY;
    if (menu->editor_index >= 3)
        return (gc_calendar_field)menu->editor_index;
    if (menu->region == GC_REGION_USA) {
        static const gc_calendar_field fields[3] = {GC_CALENDAR_MONTH, GC_CALENDAR_DAY,
                                                    GC_CALENDAR_YEAR};
        return fields[menu->editor_index];
    }
    if (menu->region == GC_REGION_JAPAN) {
        static const gc_calendar_field fields[3] = {GC_CALENDAR_YEAR, GC_CALENDAR_MONTH,
                                                    GC_CALENDAR_DAY};
        return fields[menu->editor_index];
    }
    return (gc_calendar_field)menu->editor_index;
}

static void change_calendar_field(gc_menu *menu, int direction) {
    gc_date_time *clock = &menu->clock;

    switch (gc_menu_calendar_field(menu)) {
        case GC_CALENDAR_DAY:
            clock->day = wrap_value(clock->day + direction, 1,
                                    days_in_month(clock->year, clock->month));
            break;
        case GC_CALENDAR_MONTH:
            clock->month = wrap_value(clock->month + direction, 1, 12);
            clamp_calendar_day(clock);
            break;
        case GC_CALENDAR_YEAR:
            clock->year = wrap_value(clock->year + direction, 2000, 2099);
            clamp_calendar_day(clock);
            break;
        case GC_CALENDAR_HOUR:
            clock->hour = wrap_value(clock->hour + direction, 0, 23);
            break;
        case GC_CALENDAR_MINUTE:
            clock->minute = wrap_value(clock->minute + direction, 0, 59);
            break;
        case GC_CALENDAR_SECOND:
            clock->second = wrap_value(clock->second + direction, 0, 59);
            break;
        default:
            break;
    }
}

void gc_menu_calendar_press(gc_menu *menu, gc_button button) {
    if (menu->editing) {
        if (button == GC_BUTTON_CANCEL) {
            menu->clock = menu->clock_before_edit;
            menu->editing = false;
        } else if (button == GC_BUTTON_CONFIRM) {
            /* USA/JAP 0x81327530 and PAL 0x81328ca4 read the live RTC
             * again, then replace only the tuple being edited. */
            gc_date_time clock = menu->clock_before_edit;
            if (menu->editor_index < 3) {
                clock.year = menu->clock.year;
                clock.month = menu->clock.month;
                clock.day = menu->clock.day;
            } else {
                clock.hour = menu->clock.hour;
                clock.minute = menu->clock.minute;
                clock.second = menu->clock.second;
            }
            menu->clock = clock;
            menu->clock_changed = true;
            menu->editing = false;
        } else if (button == GC_BUTTON_UP) {
            change_calendar_field(menu, 1);
        } else if (button == GC_BUTTON_DOWN) {
            change_calendar_field(menu, -1);
        } else if (button == GC_BUTTON_LEFT || button == GC_BUTTON_RIGHT) {
            unsigned row = menu->editor_index / 3;
            unsigned column = menu->editor_index % 3;
            column = button == GC_BUTTON_LEFT ? (column + 2) % 3 : (column + 1) % 3;
            menu->editor_index = row * 3 + column;
            if (row)
                menu->calendar_time_index = menu->editor_index;
            else
                menu->calendar_date_index = menu->editor_index;
        }
        return;
    }
    if (button == GC_BUTTON_CANCEL) {
        gc_menu_enter_page(menu, GC_PAGE_FACE);
    } else if (button == GC_BUTTON_CONFIRM) {
        menu->clock_before_edit = menu->clock;
        /* Native Time entry starts its Seconds draft at zero, while
         * the underlying RTC and its fractional second keep running. */
        if (menu->editor_index >= 3)
            menu->clock.second = 0;
        menu->editing = true;
    } else if (button == GC_BUTTON_UP && menu->editor_index >= 3) {
        menu->editor_index = menu->calendar_date_index;
    } else if (button == GC_BUTTON_DOWN && menu->editor_index < 3) {
        menu->editor_index = menu->calendar_time_index;
    }
}
