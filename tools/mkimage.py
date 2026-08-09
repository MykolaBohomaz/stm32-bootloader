import struct
import argparse
import zlib
from pathlib import Path

def read_file(file_path):
    with open(file_path, "rb") as file:
        #load the entire content into the memory 
        data = file.read()
        return data

def add_padding(data):
    data = bytearray(data)
    remainder = len(data) % 8
    if remainder == 0:
        return data
    else:
        padding = 8 - remainder
        data.extend(bytes([0xFF]) * padding)
        return data

def parse_version(version):
    parts = version.split(".")

    if len(parts) != 3:
        raise ValueError("Version must be MAJOR.MINOR.PATCH, e.g. 1.2.3")

    try:
        major, minor, patch = map(int, parts) 
    except ValueError:
        raise ValueError("Version must contain only numbers, e.g. 1.2.3")

    if not (0 <= major <= 255):
        raise ValueError("Major version must be 0-255")
    if not (0 <= minor <= 255):
        raise ValueError("Minor version must be 0-255")
    if not (0 <= patch <= 255):
        raise ValueError("Patch version must be 0-255")

    return ((major << 16) | (minor << 8) | patch)

def build_header(img_size, img_crc32, fw_version):
    return struct.pack(
        "<IHHIIII12s",
        0x4D494C42,  # magic
        1,            # hdr_version
        0,            # flags
        img_size,
        img_crc32,
        fw_version,
        0,            # entry_offset
        b"\x00" * 12  # reserved
    )

def write_file(file_path, header, payload):
    with open(file_path, "wb") as file:
        file.write(header)
        file.write(payload)

def main():
    #Parse command line arguments
    parser = argparse.ArgumentParser(
        description="Create an STM32 bootloader image"
    )

    parser.add_argument("input", help="Input .bin file")
    parser.add_argument("--version", required=True, help="Firmware version e.g. 1.2.3")

    args = parser.parse_args()

    print("Input:", args.input)
    print("Version:", args.version)

    #load the .bin into memory
    data = read_file(args.input)

    if len(data) == 0:
        parser.error("Input file is empty")

    #add padding 
    data = add_padding(data) 

    #compute crc of the padded payload
    data = bytes(data)
    img_crc32 = zlib.crc32(data)

    #compute firmware version
    try:
        fw_version = parse_version(args.version)
    except ValueError as e:
        parser.error(str(e))

    #Build header without crc32
    header_without_crc = build_header(len(data), img_crc32, fw_version)

    #Header crc
    hdr_crc32 = zlib.crc32(header_without_crc)

    #Finish the header 
    header = header_without_crc + struct.pack("<I", hdr_crc32)

    #Quick test
    assert len(header) == 40

    #Make the output file
    input_path = Path(args.input)

    output_path = input_path.with_suffix(".blimg")
    write_file(output_path, header, data)

    #Summary
    print("Output:", output_path)
    print("Padded size:", len(data))
    print(f"Image CRC32: 0x{img_crc32:08X}")
    print(f"Header CRC32: 0x{hdr_crc32:08X}")
    print(f"Firmware version: 0x{fw_version:08X}")


#run main
if __name__ == "__main__":
    main()


