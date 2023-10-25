#!/usr/bin/env python3

from typing import Callable

import argparse
import xml.etree.ElementTree as ElementTree


def quote(s: str):
    return f'"{s}"'


def cbool(b: bool):
    return "TRUE" if b else "FALSE"


def handle_interface(interface: ElementTree.Element):
    intf_name = interface.attrib["name"]
    for method in interface.iter("method"):
        method_name = method.attrib["name"]
        uses_requests = False
        option_arg = -1

        for pos, arg in enumerate(method.iter("arg")):
            arg_name = arg.attrib["name"]
            arg_type = arg.attrib["type"]
            arg_direction = arg.attrib.get("direction", "in")
            if (
                (arg_name == "handle" or arg_name == "request_handle")
                and arg_type == "o"
                and arg_direction == "out"
            ):
                uses_requests = True

            if arg_name == "options" and arg_type == "a{sv}" and arg_direction == "in":
                option_arg = pos

        method_name = quote(method_name)
        iname = quote(intf_name)
        print(
            f"  {{ .interface = {iname:40s}, .method = {method_name:32s}, .uses_request = {cbool(uses_requests)}, .option_arg = {option_arg:2d}, }},"
        )


def parse_portal_xml(filename: str):
    tree = ElementTree.parse(filename)
    root = tree.getroot()

    for interface in root.iter("interface"):
        handle_interface(interface)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=str, nargs="+")

    args = parser.parse_args()

    print('#include "glib.h"')
    print('#include "xdp-method-info.h"')
    print("")
    print("static const XdpMethodInfo method_info[] = {")

    for file in args.file:
        parse_portal_xml(file)

    print("  { .interface = NULL },")
    print("};")
    print("")
    print(
        "const XdpMethodInfo *xdp_method_info_get_all (void) { return method_info; };"
    )
    print("")
    print(
        "unsigned int xdp_method_info_get_count (void) { return G_N_ELEMENTS(method_info) - 1; };"
    )
