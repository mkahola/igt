#!/usr/bin/env python3
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

## Copyright (C) 2024 Advanced Micro Devices, Inc.         ##
## Author: Alex Hung <alex.hung@amd.com>                   ##

import sys, os, argparse
import numpy as np
import pprint

import pprint

class Color:
    def __init__(self, r, g, b):
        self.r = r
        self.g = g
        self.b = b
    def __str__(self):
        r = self.r
        g = self.g
        b = self.b
        rgb = "\t{ " + ".red = {0:05f}, .green = {1:05f}, .blue = {2:05f}".format(r, g, b) + " },"
        return rgb

def print_lut(ind, lut):
    for i, v in enumerate(lut):
        if (i - ind) % (17 * 17) == 0:
            print(str(i) + " " + v)
            #print(v)

def parse_lut_file(file_path, max_value=4095):
    lut = []

    try:
        with open(file_path, "r") as file:
            for line in file:
                values = line.strip().split()  # Split values by spaces or tabs
                if len(values) == 3:
                    r, g, b = map(float, values)  # Convert to floats
                    r = r / max_value
                    g = g / max_value
                    b = b / max_value
                    color = Color(r, g, b)
                    lut.append(color)
                else:
                    print(f"Skipping invalid line: '{line.strip()}'")
    except FileNotFoundError:
        print(f"File '{file_path}' not found.")

    return lut

def fill_3d_array(lut, size=17):
    lut3d = np.zeros((size, size, size), dtype=object)

    for i in range(size):
        for j in range(size):
            for k in range(size):
                index = i * (size ** 2) +  j * size + k
                color = lut[index]
                lut3d[i][j][k] = color
    return lut3d

def print_3d_array_rgb(lut3d, size=17):
    for i in range(size):
        for j in range(size):
            for k in range(size):
                print(lut3d[i][j][k])

def print_3d_array_bgr(lut3d, size=17):
    for i in range(size):
        for j in range(size):
            for k in range(size):
                print(lut3d[k][j][i])

def main():

    parser = argparse.ArgumentParser(description='Convert integer values in a 3D LUT file to IGT format.')
    parser.add_argument("-f", "--input", help="3D LUT file", required=True)
    parser.add_argument("-t", "--traversal", help="traversal order", required=True)
    args = parser.parse_args()

    lut = parse_lut_file(args.input)
    lut3d = fill_3d_array(lut)

    if args.traversal.lower() == "rgb":
        print_3d_array_rgb(lut3d)
    elif args.traversal.lower() == "bgr":
        print_3d_array_bgr(lut3d)

    return 0

if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
