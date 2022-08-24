#!/bin/bash

if [ -e .backup]; then
  chmod 600 .backup/.??*
  chmod 750 .backup/init
  chmod 700 .backup

  chmod 750 overlay.d
  chmod 750 overlay.d/sbin
  chmod 644 overlay.d/sbin/*
fi

chmod 750 init
chmod 744 second_stage_resources
