# SPDX-License-Identifier: MIT
## Copyright (C) 2026    Intel Corporation                 ##

import json

print("Hope you've got enough memory for this! Generating text...")
PREFIX = "it's time for " + ("A" * 1024 * 1024 * 1024 * 4)

print("Writing to out.txt...")
with open('0/out.txt', 'w') as f:
	f.write(PREFIX)

print("Loading reference.json...")
with open('reference.json', 'r') as f:
	data = json.load(f)

print("Writing new reference.json...")
with open('reference.json', 'w') as f:
	data['tests']['igt@successtest@first-subtest']['out'] = PREFIX
	json.dump(data, f)

print("Removing SKIP_ME...")
os.unlink("SKIP_ME")
