#!/usr/bin/python3
import sys
import json
import re

'''
Parsing support for the following command:
sudo time -v numactl -N 0 \
        perf stat -e dsa0/event=0x1,event_category=0x0/,\
                     dsa2/event=0x1,event_category=0x0/,\
                     dsa4/event=0x1,event_category=0x0/,\
                     dsa6/event=0x1,event_category=0x0/,\
                     dsa8/event=0x1,event_category=0x0/ \
        opt/cachelib/bin/cachebench --json_test_config <config_json> \
                     --report_api_latency
'''
def num(s):
    try:
        return int(s)
    except ValueError:
        return float(s)


def parse(fileName):
    with open(fileName) as file:
        data = file.readlines()

    ## Parse config
    config = []
    seen = False
    for line in data:
        line.strip()
        if line == "{\n":
            seen = True
        elif line == "Welcome to OSS version of cachebench\n":
            break
        elif "reading distribution params from" in line or line == '\n':
            continue
        if seen:
            config.append(line)

    jconfig = json.loads("".join(config))

    ## Split cachebench and system results
    cbResults = []
    sysResults = []
    seen = 0
    for line in data:
        line.strip()
        if "== Test Results ==" in line:
            seen = 1
            continue
        elif "Performance counter stats for 'system wide'" in line:
            seen = 2
            continue
        elif "== KVReplayGenerator Stats ==" in line:
            seen = 0
            continue

        if seen == 1:
            cbResults.append(line)
        elif seen == 2:
            sysResults.append(line)

    ## Parse cachebench metrics
    cbMetrics = {}
    for line in cbResults:
        if ':' in line:
            key, value, *_ = [x.rstrip(', \n').replace(" ","_").lower() for x in line.split(':') if not len(x) == 2]
            key = re.sub('__+' , '_', key)
            key = re.sub('[^\d\w]', '', key)

            value = re.sub('[^0-9.]', '', value)
            value = num(value)

            cbMetrics[key] = value

    cbMetrics = {'cachebench_metrics': cbMetrics}

    ## Parse system metrics
    sysMetrics = {}
    for line in sysResults:
        if "Elapsed (wall clock) time" in line:
            continue

        elif "numactl" in line:
            value = line.split(':', 1)[-1].strip().replace('"','')
            sysMetrics["command"] = value

        elif "seconds time elapsed" in line:
            value, key = [x.strip().lower() for x in line.split('seconds')]
            sysMetrics["time_elapsed_in_secs"] = num(value.strip())

        elif ",event_category=0x" in line:
            value, key = [x.strip().lower() for x in line.split('dsa')]
            key = "dsa" + key.strip()
            sysMetrics[key] = num(value.strip())

        elif ':' in line:
            key, value, *_ = [x.rstrip(', \n').replace(" ","_").lower() for x in line.split(':') if not len(x) == 2]
            key = re.sub('__+' , '_', key)
            key = re.sub('[^\d\w]', '', key)
            value = re.sub('[^0-9.]', '', value)
            value = num(value)
            sysMetrics[key] = value

    sysMetrics = {'system_metrics': sysMetrics}

    ## Create the unified view
    joined = {**jconfig, **cbMetrics, **sysMetrics}
    return json.dumps(joined, indent=2)


def main():
    args = sys.argv[1:]
    fileName = args[0]
    out = parse(fileName)
    print(out)


if __name__ == '__main__':
    main()
