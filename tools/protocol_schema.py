"""Load the emulator's recovered protobuf descriptor set in dependency order."""

from google.protobuf import descriptor_pb2, descriptor_pool


def load_descriptor_pool(descriptor_path):
    descriptor_set = descriptor_pb2.FileDescriptorSet.FromString(descriptor_path.read_bytes())
    pending = {descriptor.name: descriptor for descriptor in descriptor_set.file}
    if len(pending) != len(descriptor_set.file):
        raise ValueError("Recovered descriptor set contains duplicate file names")

    pool = descriptor_pool.DescriptorPool()
    loaded = set()
    while pending:
        ready = [descriptor for descriptor in pending.values()
                 if all(dependency in loaded for dependency in descriptor.dependency)]
        if not ready:
            unresolved = ", ".join(sorted(pending))
            raise ValueError("Recovered descriptor dependencies cannot be resolved: " + unresolved)

        for descriptor in ready:
            pool.AddSerializedFile(descriptor.SerializeToString())
            loaded.add(descriptor.name)
            del pending[descriptor.name]

    return pool
