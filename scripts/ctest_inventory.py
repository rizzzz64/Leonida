"""Report documented CTest omissions; does not run tests or change CI gates.

Usage: python -I scripts/ctest_inventory.py --build runtime/build --config RelWithDebInfo
Use --windows-vulkan for a Windows runtime configuration with Vulkan enabled.
The report covers the prerequisite-sensitive groups documented in docs/testing.md,
not every conditional test or partial check. Registration is not execution.
"""
import argparse
import json
import subprocess

GROUPS = {
    'llvm-mc + spirv-as + spirv-val': (
        'd3d12_translated_draw', 'd3d12_m2_textured', 'd3d12_m2_formats',
        'd3d12_m3_frame', 'd3d12_m4_compute', 'd3d12_buffer_uav'),
    'llvm-mc + glslangValidator': ('d3d12_subgroup',),
    'glslangValidator': ('d3d12_compute', 'd3d12_draw', 'd3d12_set0_compute', 'd3d12_mesh'),
}


def inventory(document, groups):
    if document['kind'] != 'ctestInfo' or document['version']['major'] != 1:
        raise ValueError('unsupported CTest inventory format')
    tests = document['tests']
    names = {test['name'] for test in tests}
    disabled = {test['name'] for test in tests if any(
        prop['name'] == 'DISABLED' and prop['value'] is True
        for prop in test.get('properties', []))}
    return {
        'registered_count': len(tests),
        'disabled': sorted(disabled),
        'groups': {requirement: {'absent': sorted(set(expected) - names),
                                 'disabled': sorted(set(expected) & disabled)}
                   for requirement, expected in groups.items()},
        'execution_verified': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    parser.add_argument('--config', default='RelWithDebInfo')
    parser.add_argument('--windows-vulkan', action='store_true',
                        help='compare documented Windows Vulkan test groups')
    args = parser.parse_args()
    try:
        result = subprocess.run(['ctest', '--test-dir', args.build, '-C', args.config,
                                 '--show-only=json-v1'], check=True, capture_output=True, text=True)
        report = inventory(json.loads(result.stdout), GROUPS if args.windows_vulkan else {})
    except (OSError, subprocess.CalledProcessError, ValueError, KeyError, TypeError) as exc:
        parser.exit(2, f'Cannot read CTest inventory: {exc}\n')
    report['scope'] = 'documented Windows Vulkan groups' if args.windows_vulkan else 'registration only'
    report['note'] = ('Absent tests may indicate missing prerequisites or other configuration conditions; '
                      'inspect configure output. Registered tests may still skip or fail.')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
