#!/bin/bash

wc -l core/*.c include/*.h plugins/method/irc/{*.c,*.h} plugins/service/openweather/{*.c,*.h} plugins/bot/command/{*.c,*.h} plugins/db/postgresql/{*.c,*.h}

exit 0
