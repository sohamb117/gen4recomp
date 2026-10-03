# Six hours later on the same day: the per-minute systems advance and the daily
# ones do not. Every number below is arithmetic rather than a recording, the
# honey tree is 1440 - 360, the berry crosses two of its 180-minute stages, and
# Cheri drains 15 moisture an hour, so 100 - 6*15 = 10.
#
# Shaymin stays in its Sky form because the daytime branch reverts it only once
# more minutes have passed than the day is old, and 16:00 is 720 minutes past
# the 04:00 the branch counts from.
#
# Pinned after two identical runs and one under an empty
# environment. Regenerate with pc/tests/pc_corpus.py --pin.
input:   pc/replays/lab-continue.txt
frames:  3200
rtc:     2009-03-22 16:00:00
save-at: 2800
digest:  9C3401F5176191A1
flag:    FLAG_DAILY_WON_AGAINST_VALLEY_WINDWORKS_OUTSIDE_DRIFLOON=1
var:     VAR_DAILY_RANDOM_LEVEL=96 VAR_LOTTERY_TRAINER_ID_LOW_HALF=56143
honey:   0=1080
berry:   0.growthStage=BERRY_GROWTH_STAGE_GROWING 0.moistureRating=10
form:    0=SHAYMIN_FORM_SKY
name:    CLOCKER
map:     3
pos:     180,777
money:   3000
badges:  0
party:   1
