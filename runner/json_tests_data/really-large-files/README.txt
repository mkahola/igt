Check if an incredibly large JSON is generated and tested properly.
This requires a large out.txt file to be available - it's not in
the repo directly, run generate_data.py to test that properly.
The SKIP_ME marker file exists to skip this test by default.

The logic for small/large files does not change, testing this for each
commit takes a long time and does not really check anything special.
