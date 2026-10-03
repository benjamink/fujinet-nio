from __future__ import annotations

import argparse
import unittest

from fujinet_tools import net, netproto as np


class ImageTranslationTests(unittest.TestCase):
    def test_image_translation_constant(self) -> None:
        self.assertEqual(np.TRANSLATION_IMAGE, 4)

    def test_open_request_carries_image_selector_extension(self) -> None:
        selector = "w=640,h=400,colors=16"
        req = np.build_open_req(
            method=1,
            flags=0,
            url="http://x/a.png",
            translation_type=np.TRANSLATION_IMAGE,
            translation_selector=selector,
        )
        sel = selector.encode()
        expected = (
            b"\x01\x01\x00"  # version, method GET, flags
            + b"\x0e\x00http://x/a.png"
            + b"\x00\x00"  # header count
            + b"\x00\x00\x00\x00"  # body length hint
            + b"\x00\x00"  # response header count
            + b"\x01\x00\x00\x00"  # openExtFlags = translation
            + b"\x04\x00"  # translationType image, flags 0
            + bytes([len(sel), 0])
            + sel
        )
        self.assertEqual(req, expected)

    def test_translate_configure_request_for_image(self) -> None:
        req = np.build_translate_configure_req(
            7,
            translation_type=np.TRANSLATION_IMAGE,
            translation_selector="w=320",
        )
        self.assertEqual(req, b"\x01\x07\x00\x04\x00\x05\x00w=320")

    def test_cli_maps_image_to_4(self) -> None:
        self.assertEqual(net._translation_type_value("image"), 4)
        self.assertEqual(net._translation_type_value("IMAGE"), 4)

    def test_cli_accepts_image_content_type_everywhere(self) -> None:
        parser = argparse.ArgumentParser()
        sub = parser.add_subparsers()
        net.register_subcommands(sub)
        for argv in (
            ["net", "open", "--content-type", "image", "u"],
            ["net", "get", "--content-type", "image", "u"],
            ["net", "head", "--content-type", "image", "u"],
            ["net", "send", "--method", "2", "--content-type", "image", "u"],
            ["net", "translate", "--handle", "1", "--content-type", "image"],
        ):
            ns = parser.parse_args(argv)
            self.assertEqual(ns.content_type, "image")


if __name__ == "__main__":
    unittest.main()
