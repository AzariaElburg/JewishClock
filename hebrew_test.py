def is_hebrew_leap(year):
    return ((7*year+1)%19) < 7

def hebrew_elapsed_days(year):
    months = (235*(year-1)-234) // 19
    parts = 12084 + 13753 * months
    day = months*29 + parts // 25920
    if ((3*(day+1)) % 7) < 3:
        day += 1
    return day

def days_in_hebrew_year(year):
    return hebrew_elapsed_days(year+1) - hebrew_elapsed_days(year)

def days_in_hebrew_month(month, year):
    if month == 1: return 30
    if month == 2: return 29
    if month == 3: return 30
    if month == 4: return 29
    if month == 5: return 30
    if month == 6: return 29
    if month == 7: return 30
    if month == 8: return 30 if days_in_hebrew_year(year) % 10 == 5 else 29
    if month == 9: return 29 if days_in_hebrew_year(year) % 10 == 3 else 30
    if month == 10: return 29
    if month == 11: return 30
    if month == 12: return 30 if is_hebrew_leap(year) else 29
    if month == 13: return 29
    return 29

def hebrew_to_absolute(y,m,d):
    abs_day = d
    TISHREI=7; NISSAN=1; ADAR=12; ADAR_II=13
    if m < TISHREI:
        endm = ADAR_II if is_hebrew_leap(y) else ADAR
        for i in range(TISHREI, endm+1):
            abs_day += days_in_hebrew_month(i,y)
        for i in range(NISSAN, m):
            abs_day += days_in_hebrew_month(i,y)
    else:
        for i in range(TISHREI, m):
            abs_day += days_in_hebrew_month(i,y)
    return abs_day + hebrew_elapsed_days(y) - 1 + 1373428

def gregorian_to_absolute(y,m,d):
    leap = (y%4==0 and y%100!=0) or (y%400==0)
    days = [0,31,28,31,30,31,30,31,31,30,31,30,31]
    abs_day = d + 365*(y-1) + (y-1)//4 - (y-1)//100 + (y-1)//400
    for i in range(1,m):
        abs_day += days[i]
        if i == 2 and leap: abs_day += 1
    return abs_day

def absolute_to_hebrew(abs_day):
    y = int((abs_day - 1373428) * 19 / 6935) + 1
    while hebrew_to_absolute(y+1,7,1) <= abs_day:
        y += 1
    m = 7 if abs_day < hebrew_to_absolute(y,1,1) else 1
    while abs_day > hebrew_to_absolute(y,m,days_in_hebrew_month(m,y)):
        m += 1
    d = abs_day - hebrew_to_absolute(y,m,1) + 1
    return y,m,d

for y,m,d in [(2026,3,31),(2026,4,1),(2026,4,15),(2026,4,22)]:
    absd = gregorian_to_absolute(y,m,d)
    hy,hm,hd = absolute_to_hebrew(absd)
    print(y,m,d,'->',hy,hm,hd)
