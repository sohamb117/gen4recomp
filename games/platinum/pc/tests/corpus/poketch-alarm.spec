# Alarm clock. Raise the hour once and set it.
#
# Pinned after two identical runs and one under an empty
# environment. Regenerate with pc/tests/pc_corpus.py --pin.
input:   pc/replays/lab-poketch-alarm.txt
frames:  4000
rtc:     2009-03-22 06:00:00
rtc2:    2009-03-22 21:00:00
rtc-diff: 1
save-at: 3600
digest:  4336BE8EA2AAEB81
digest2: EC5C252F7AE142F1
poketch: enabled=1 app=POKETCH_APPID_ALARMCLOCK alarm=1 alarm-hour=5 alarm-minute=0
name:    WATCHER
map:     3
pos:     180,777
money:   3000
badges:  0
party:   1
