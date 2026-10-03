# The control: booted on the clock its recipe minted it on, so no time has
# passed and nothing clock-driven may have moved. Without this station every
# assert in the other three would also pass on a port that ran the daily and
# per-minute handlers at every boot regardless of the clock.
#
# Pinned after two identical runs and one under an empty
# environment. Regenerate with pc/tests/pc_corpus.py --pin.
input:   pc/replays/lab-continue.txt
frames:  3200
rtc:     2009-03-22 10:00:00
save-at: 2800
digest:  601B9FCFFCC5D223
flag:    FLAG_DAILY_WON_AGAINST_VALLEY_WINDWORKS_OUTSIDE_DRIFLOON=1
var:     VAR_DAILY_RANDOM_LEVEL=96 VAR_LOTTERY_TRAINER_ID_LOW_HALF=56143
honey:   0=1440
berry:   0.growthStage=BERRY_GROWTH_STAGE_PLANTED 0.moistureRating=100
form:    0=SHAYMIN_FORM_SKY
name:    CLOCKER
map:     3
pos:     180,777
money:   3000
badges:  0
party:   1
