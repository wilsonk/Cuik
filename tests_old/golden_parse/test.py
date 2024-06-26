import os
import re
import subprocess

tests_done = 0
tests_passed = 0

def handle_file(filepath, options):
    global tests_passed
    global tests_done
    tests_done += 1
    
    print(filepath + "... ", end='')
    
    result = subprocess.run(['cuik', filepath] + options, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode == 0:
        print("Success!")
        tests_passed += 1
        return
    
    print("Failed!")

for file in os.listdir("./"):
    handle_file(file, ["-xe"])

print(f"Passed {tests_passed} of {tests_done}!")
