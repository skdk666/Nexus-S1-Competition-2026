"""Create an ignored local build header; never overwrite an existing configuration."""
from pathlib import Path
import secrets

root=Path(__file__).resolve().parents[1]
target=root/'Nexus_S1_Firmware/include/local_secrets.h'
if target.exists():
    raise SystemExit('Local configuration already exists; kept unchanged.')
password=secrets.token_urlsafe(24)
with target.open('x',encoding='utf-8',newline='\n') as stream:
    stream.write('#pragma once\n#define NEXUS_WIFI_AP_PASSWORD "'+password+'"\n'
                 '#define NEXUS_MQTT_HOST "broker.invalid"\n#define NEXUS_MQTT_PORT 1883\n')
print('Created ignored local configuration. Password is not printed. Read/edit locally if needed.')
