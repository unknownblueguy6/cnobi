import re

# Function to parse the build.ninja file and extract the necessary information
def parse_ninja_file(ninja_file):
    rules = {}
    targets = []

    with open(ninja_file, 'r') as f:
        lines = f.readlines()

    current_rule = None
    current_bindings = []
    
    # Regular expressions for parsing relevant information
    rule_pattern = re.compile(r'rule\s+(\w+)')
    command_pattern = re.compile(r'command\s*=\s*(.*)')
    build_pattern = re.compile(r'build\s+(\S+):\s*(\S+)\s+(.*)')
    flag_pattern = re.compile(r'\s*(\S+)\s*=\s*(\S+)')

    for line in lines:
        line = line.strip()

        # Parse rule definitions
        match_rule = rule_pattern.match(line)
        if match_rule:
            current_rule = match_rule.group(1)
            rules[current_rule] = {'command': '', 'bindings': []}
            continue

        # Parse rule commands
        match_command = command_pattern.match(line)
        if match_command and current_rule:
            rules[current_rule]['command'] = match_command.group(1)
            continue

        # Parse build targets
        match_build = build_pattern.match(line)
        if match_build:
            target = match_build.group(1)
            rule = match_build.group(2)
            dependencies = match_build.group(3).split()
            targets.append({'target': target, 'rule': rule, 'dependencies': dependencies})
            continue

        # Parse flags and bindings for targets
        match_flag = flag_pattern.match(line)
        if match_flag and current_rule:
            flag_name = match_flag.group(1)
            flag_value = match_flag.group(2)
            rules[current_rule]['bindings'].append((flag_name, flag_value))
    
    return rules, targets

# Function to generate the manifest.c file
def generate_manifest_c(rules, targets, output_file="manifest.c"):
    with open(output_file, 'w') as f:
        f.write('#include "manifest.h"\n\n')
        
        # Generate RULEs for compile and link
        for rule_name, rule_data in rules.items():
            f.write(f"RULE({rule_name})\n")
            f.write(f"  .command = START_EVAL L({rule_data['command']}) END_EVAL,\n")
            f.write("END_RULE\n\n")

        # Generate BINDINGS for flags
        f.write("Manifest manifest = {\n")
        f.write("  BINDINGS\n")
        for rule_name, rule_data in rules.items():
            for flag_name, flag_value in rule_data['bindings']:
                f.write(f"    BL({flag_name}, \"{flag_value}\")\n")
        f.write("  END_BIND,\n\n")

        # Generate EDGES for build targets
        f.write("  .edges =\n")
        f.write("    START_EDGE\n")
        for target in targets:
            f.write("    {\n")
            f.write(f"      .rule = &{target['rule']},\n")
            f.write(f"      .in = START_EVAL L({' '.join(target['dependencies'])}) END_EVAL,\n")
            f.write(f"      .out = START_EVAL L({target['target']}) END_EVAL,\n")
            f.write("    },\n")
        f.write("  END_EDGE\n")
        f.write("};\n")

    print(f"Manifest generated: {output_file}")

# Main function to convert build.ninja to manifest.c
def convert_ninja_to_manifest(ninja_file, output_file="manifest.c"):
    rules, targets = parse_ninja_file(ninja_file)
    generate_manifest_c(rules, targets, output_file)

# Example usage
if __name__ == "__main__":
    ninja_file = 'build.ninja'  # Path to your build.ninja file
    output_file = 'manifest2.c'  # Desired output file name
    convert_ninja_to_manifest(ninja_file, output_file)

