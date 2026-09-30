import unittest

from fujinet_tools import disk
from fujinet_tools import diskproto as dp


class DiskImageTypeTests(unittest.TestCase):
    def test_every_type_round_trips_between_name_and_wire_value(self):
        for value, name in disk.TYPE_TEXT.items():
            self.assertEqual(disk._type_parse(name), value)
            self.assertEqual(disk._type_str(value), name)

    def test_diskcopy42_matches_cpp_image_type(self):
        # disk::ImageType::DiskCopy42 in include/fujinet/disk/disk_types.h
        self.assertEqual(dp.TYPE_DC42, 5)
        self.assertEqual(disk._type_parse("DC42"), dp.TYPE_DC42)

    def test_unknown_type_is_rejected(self):
        with self.assertRaises(ValueError):
            disk._type_parse("woz")


if __name__ == "__main__":
    unittest.main()
