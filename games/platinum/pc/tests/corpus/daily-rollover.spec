# The next day: FieldSystem_HandleDailyEvents fires on top of the per-minute
# handlers. The daily flag the recipe set is cleared, the Jubilife lottery
# number is re-rolled and the daily random level with it; the honey tree's
# countdown is clamped at zero rather than going negative, and the berry patch
# has run dry and lost yield.
#
# The lottery's high half is expected to stay zero. SetJubilifeLotteryTrainerID
# writes both halves of the number into VAR_LOTTERY_TRAINER_ID_LOW_HALF and
# never touches the high one, so the ID the game reads back is always under
# 65536. That is the game's own behaviour, and pinning it is how a later change
# that "fixes" it gets noticed.
#
# Pinned after two identical runs and one under an empty
# environment. Regenerate with pc/tests/pc_corpus.py --pin.
input:   pc/replays/lab-continue.txt
frames:  3200
rtc:     2009-03-23 13:00:00
save-at: 2800
digest:  0B660C0AA4418144
flag:    FLAG_DAILY_WON_AGAINST_VALLEY_WINDWORKS_OUTSIDE_DRIFLOON=0
var:     VAR_DAILY_RANDOM_LEVEL=3 VAR_LOTTERY_TRAINER_ID_LOW_HALF=41696 VAR_LOTTERY_TRAINER_ID_HIGH_HALF=0
honey:   0=0
berry:   0.growthStage=BERRY_GROWTH_STAGE_GROWING 0.moistureRating=0 0.yieldRating=2
form:    0=SHAYMIN_FORM_LAND
name:    CLOCKER
map:     3
pos:     180,777
money:   3000
badges:  0
party:   1
