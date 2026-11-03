"""Generate typed C++ auth/lobby/system classes from authoritative descriptors."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from google.protobuf import descriptor_pb2


ROOTS = [
    "p_c2a.proto",
    "p_a2c.proto",
    "p_c2a.ws.proto",
    "p_a2c.ws.proto",
    "p_c2f.ws.proto",
    "p_f2c.ws.proto",
    # Front WebSocket guild requests/responses; p_guild.proto follows as a dependency.
    "p_c2f.ws.guild.proto",
    "p_f2c.ws.guild.proto",
    # World-transport guild invitations and unit guild notifications.
    "p_c2s.guild.proto",
    "p_s2c.guild.proto",
    # Front REST character preferences (CharHotKeyLoadReq/CharHotKeySaveReq quick slots).
    "p_c2f.rest.proto",
    # Native mailbox list responses and MailContent attachment payloads.
    "p_f2c.rest.proto",
    # Agent-engine (AI chat) REST requests and responses; the client exchanges them as JSON.
    "p_c2ae.rest.proto",
    "p_ae2c.rest.proto",
    # The registered market service includes these typed world request/reply headers.
    "p_c2s.market.proto",
    "p_s2c.market.proto",
    "p_system.proto",
]


def collect_schema_names(descriptors):
    files_by_name = {file.name: file for file in descriptors.file}
    needed = set()

    def visit(name):
        if name in needed:
            return

        needed.add(name)
        for dependency in files_by_name[name].dependency:
            visit(dependency)

    for root in ROOTS:
        visit(root)

    return sorted(name for name in needed if not name.startswith("google/protobuf/"))


def patch_enum_name_helper(output_directory, schema_names):
    # Rename only the C++ callable that collides with the recovered enum value.
    # Enum values, serialized descriptors, JSON names, and wire values stay intact.
    helper_pattern = re.compile(r"\bEX5RecipeSortType_Name(?=\s*\()")
    replacements = 0

    for generated_file in sorted(output_directory.glob("*.pb.*")):
        text = generated_file.read_text(encoding="utf-8")
        patched, count = helper_pattern.subn("EX5RecipeSortType_ValueName", text)
        if count:
            generated_file.write_text(patched, encoding="utf-8")
            replacements += count

    if "cms.generated.proto" in schema_names and replacements == 0:
        raise RuntimeError(
            "Expected CMS helper collision shim was not applied; inspect protoc output"
        )

    return replacements


def generate_sources(descriptor_path, protoc, output_directory, schema_names):
    output_directory.mkdir(parents=True, exist_ok=True)
    # Preserve timestamps of unchanged units so adding a schema does not rebuild
    # the entire recovered CMS library and every handwritten consumer.
    with tempfile.TemporaryDirectory(prefix="chronicles-protos-") as temporary:
        staging_directory = Path(temporary)
        command = [
            protoc,
            "--descriptor_set_in=" + str(descriptor_path),
            "--cpp_out=" + str(staging_directory),
            *schema_names,
        ]
        subprocess.run(command, check=True)
        replacements = patch_enum_name_helper(staging_directory, schema_names)
        for generated_file in staging_directory.glob("*.pb.*"):
            destination = output_directory / generated_file.name
            if destination.exists() and destination.read_bytes() == generated_file.read_bytes():
                continue
            shutil.copyfile(generated_file, destination)

    return replacements


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--protoc")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    descriptors = descriptor_pb2.FileDescriptorSet.FromString(args.descriptor.read_bytes())
    schema_names = collect_schema_names(descriptors)
    if args.list:
        print(";".join(schema_names))
    else:
        replacements = generate_sources(
            args.descriptor, args.protoc, args.output, schema_names
        )
        print(
            f"Generated {len(schema_names)} schema units; "
            f"adjusted {replacements} C++ helper references"
        )


if __name__ == "__main__":
    main()
