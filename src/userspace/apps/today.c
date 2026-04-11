#include "box/io.h"
#include "box/time.h"
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
    time_t now;
    int rc = time_get(&now);
    if (rc != 0)
    {
        println("today: failed to read RTC");
        exit(1);
    }

    char buf[20];
    time_format(&now, buf, sizeof(buf));

    color(COLOR_LIGHT_CYAN);
    printf("%s", buf);

    color(COLOR_YELLOW);
    printf("  %s", weekday_name(now.weekday));

    color(COLOR_LIGHT_GRAY);
    println("");

    exit(0);
    return 0;
}
