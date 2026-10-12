#include "box/print.h"
#include "box/clock.h"
#include "box/system.h"

static const char *weekday_name(uint8_t wd)
{
    switch (wd)
    {
    case 0:
        return "Sunday";
    case 1:
        return "Monday";
    case 2:
        return "Tuesday";
    case 3:
        return "Wednesday";
    case 4:
        return "Thursday";
    case 5:
        return "Friday";
    case 6:
        return "Saturday";
    default:
        return "Unknown";
    }
}

int main(void)
{
    BoxTime now;
    int rc = clock_boxtime_local(&now);
    if (rc != 0)
    {
        println("today: failed to read local time");
        exit(1);
    }

    char buf[20];
    time_format(&now, buf, sizeof(buf));

    set_color(COLOR_CYAN);
    print(buf);
    set_color(COLOR_YELLOW);
    print("  ");
    println(weekday_name(now.weekday));
    set_color(COLOR_DEFAULT);

    exit(0);
    return 0;
}
