#!/usr/bin/env python3
class Manifest:
    """Stores the parsed contents of a build.ninja file."""
    
    def __init__(self):
        self.pools = {}    # name -> {'depth': N}
        self.rules = {}    # name -> {bindings}
        self.bindings = {} # name -> value
        self.edges = []    # list of build edges
        self.defaults = [] # list of default targets
        self.last_line = None  # Store the last valid line
        
    def parse_file(self, filepath):
        """Parse a .ninja file and populate the manifest."""
        with open(filepath, 'r', encoding='utf-8') as f:
            self._parse_contents(f, filepath)
    
    def _read_line(self, f):
        """Read a line from file, handling comments and line continuations."""
        if self.last_line is not None:
            line = self.last_line
            self.last_line = None
            return line
            
        line_continuation = []
        
        while True:
            line = f.readline()
            if not line:  # EOF
                if line_continuation:
                    return ''.join(line_continuation)
                return None
            
            # Handle comments
            comment_pos = line.find('#')
            if comment_pos >= 0:
                line = line[:comment_pos]
            
            # Handle line continuation
            if line_continuation:
                line = line.lstrip()
            
            if line.endswith("$\n"):
                line_continuation.append(line[:-2])
                continue
            
            if line_continuation:
                line_continuation.append(line.rstrip())
                return ''.join(line_continuation)
            
            line = line.rstrip()
            if not line:
                continue
            
            return line
    
    def _parse_contents(self, f, filepath):
        """Parse the contents of a .ninja file line by line."""
        while True:
            line = self._read_line(f)
            if line is None:  # EOF
                break
            
            # Skip empty lines
            if not line.strip():
                continue
            
            if line.startswith('pool '):
                self._parse_pool(f, line)
            elif line.startswith('rule '):
                self._parse_rule(f, line)
            elif line.startswith('build '):
                self._parse_edge(f, line)
            elif line.startswith('default '):
                parts = self._split_on_unescaped_spaces(line)
                self.defaults.extend(parts[1:])  # Skip 'default'
            elif line.startswith('include '):
                self._handle_include(line[8:].strip(), filepath)
            elif line.startswith('subninja '):
                raise ValueError("subninja is not supported")
            elif '=' in line:  # Only parse as binding if it contains =
                self._parse_binding(line, self.bindings)
            else:
                # Don't ignore other lines - they might be important
                print(f"Warning: Unhandled line: {line}")
    
    def _is_indented(self, line):
        """Return True if the line starts with any whitespace."""
        return line and (line[0].isspace())
    
    def _parse_pool(self, f, line):
        """Parse a pool declaration."""
        tokens = line.split()
        pool_name = tokens[1]
        self.pools[pool_name] = {'depth': None}
        
        while True:
            line = self._read_line(f)
            if not line:  # EOF
                break
            if not self._is_indented(line):
                self.last_line = line  # Store the line for next read
                break
            line = line.strip()
            if line.startswith('depth = '):
                self.pools[pool_name]['depth'] = int(line.split('=')[1].strip())
    
    def _parse_rule(self, f, line):
        """Parse a rule declaration."""
        tokens = line.split()
        rule_name = tokens[1]
        self.rules[rule_name] = {}
        
        while True:
            line = self._read_line(f)
            if not line:  # EOF
                break
            if not self._is_indented(line):
                self.last_line = line  # Store the line for next read
                break
            self._parse_binding(line.strip(), self.rules[rule_name])
    
    def _split_on_unescaped_spaces(self, s):
        """Split string on spaces that aren't preceded by $."""
        parts = []
        current = []
        i = 0
        while i < len(s):
            if s[i] == '$' and i + 1 < len(s) and s[i + 1] == ' ':
                current.append(' ')
                i += 2
            elif s[i].isspace():
                if current:
                    parts.append(''.join(current))
                    current = []
                i += 1
            else:
                current.append(s[i])
                i += 1
        if current:
            parts.append(''.join(current))
        return parts
    
    def _parse_edge(self, f, line):
        """Parse a build edge."""
        out_in = line.split(':')
        if len(out_in) != 2:
            raise ValueError(f"Invalid build line: {line}")
        
        # Parse outputs section
        out_parts = self._split_on_unescaped_spaces(out_in[0])
        outputs = []
        implicit_outputs = []
        current = outputs
        
        # Skip 'build'
        for out in out_parts[1:]:
            if out.startswith('|'):
                if len(out) > 1:
                    implicit_outputs.append(out[1:])
                current = implicit_outputs
            else:
                current.append(out)
        
        # Parse rule and inputs section
        in_parts = self._split_on_unescaped_spaces(out_in[1].strip())
        if not in_parts:
            raise ValueError(f"Missing rule in build line: {line}")
        
        rule_name = in_parts[0]
        inputs = []
        implicit_inputs = []
        order_only = []
        validation_inputs = []
        current = inputs
        
        # Process all inputs after the rule name
        for in_file in in_parts[1:]:
            if in_file.startswith('|@'):
                if len(in_file) > 2:
                    validation_inputs.append(in_file[2:])
                current = validation_inputs
            elif in_file.startswith('||'):
                if len(in_file) > 2:
                    order_only.append(in_file[2:])
                current = order_only
            elif in_file.startswith('|'):
                if len(in_file) > 1:
                    implicit_inputs.append(in_file[1:])
                current = implicit_inputs
            else:
                current.append(in_file)
        
        # Create the edge object
        edge = {
            'outputs': outputs,
            'implicit_outputs': implicit_outputs,
            'rule': rule_name,
            'inputs': inputs,
            'implicit_inputs': implicit_inputs,
            'validation_inputs': validation_inputs,
            'order_only': order_only,
            'pool': None,
            'bindings': {}
        }
        
        # Parse indented bindings
        while True:
            line = self._read_line(f)
            if not line:  # EOF
                break
            if not self._is_indented(line):
                self.last_line = line  # Store the line for next read
                break
            line = line.strip()
            if line.startswith('pool = '):
                edge['pool'] = line.split('=', 1)[1].strip()
            else:
                self._parse_binding(line, edge['bindings'])
        
        self.edges.append(edge)
    
    def _parse_binding(self, line, bindings_dict):
        """Parse a variable binding."""
        parts = line.split('=', 1)
        name = parts[0].strip()
        value = parts[1].strip() if len(parts) > 1 else ''
        if value:  # Only add non-empty bindings
            bindings_dict[name] = value
    
    def _handle_include(self, include_path, current_file):
        """Handle include or subninja statement."""
        import os.path
        base_dir = os.path.dirname(current_file)
        full_path = os.path.join(base_dir, include_path)
        self.parse_file(full_path)
        return 1
    
    def display_manifest(self):
        """Display the manifest data structure."""
        print("Pools:")
        for name, pool in self.pools.items():
            print(f"  {name}:")
            print(f"    depth: {pool['depth']}")
        print()
        
        print("Rules:")
        for name, bindings in self.rules.items():
            print(f"  {name}:")
            for key, value in bindings.items():
                print(f"    {key}: {value}")
        print()
        
        print("Bindings:")
        for name, value in self.bindings.items():
            print(f"  {name}: {value}")
        print()
        
        print("Edges:")
        for edge in self.edges:
            print("  {")
            print(f"    outputs: {edge['outputs']}")
            print(f"    implicit_outputs: {edge['implicit_outputs']}")
            print(f"    rule: {edge['rule']}")
            print(f"    inputs: {edge['inputs']}")
            print(f"    implicit_inputs: {edge['implicit_inputs']}")
            print(f"    validation_inputs: {edge['validation_inputs']}")
            print(f"    order_only: {edge['order_only']}")
            print(f"    pool: {edge['pool']}")
            print("    bindings: {")
            for key, value in edge['bindings'].items():
                print(f"      {key}: {value}")
            print("    }")
            print("  }")
        print()
        
        print("Defaults:")
        print(f"  {self.defaults}")
    
    def _is_var_char(self, c):
        """Check if a character is valid in a variable name.
        Variables can contain alphanumeric chars, underscore, dash, and dot.
        """
        return c.isalnum() or c in '_-.'
    
    def _tokenize_string(self, s):
        """Break a string into literals and variables.
        Returns a list of tuples (type, value) where type is 'L' for literal or 'V' for variable.
        """
        tokens = []
        current = []
        i = 0
        while i < len(s):
            if s[i] == '$':
                # First, emit current literal if any
                if current:
                    tokens.append(('L', ''.join(current)))
                    current = []
                    
                i += 1  # Skip the $
                
                # Handle different $ cases
                if s[i] == '$':  # Literal $
                    current.append('$')
                    i += 1
                elif s[i] == ' ':  # Literal space
                    current.append(' ')
                    i += 1
                elif s[i] == ':':  # Literal colon
                    current.append(':')
                    i += 1
                elif s[i] == '{':  # ${var}
                    i += 1
                    var = []
                    while i < len(s) and s[i] != '}':
                        var.append(s[i])
                        i += 1
                    if i < len(s):  # Skip closing }
                        i += 1
                    if var:
                        tokens.append(('V', ''.join(var)))
                else:  # $var
                    var = []
                    while i < len(s) and self._is_var_char(s[i]):
                        var.append(s[i])
                        i += 1
                    if var:
                        tokens.append(('V', ''.join(var)))
            else:
                current.append(s[i])
                i += 1
                
        # Emit final literal if any
        if current:
            tokens.append(('L', ''.join(current)))
            
        return tokens
    
    def _escape_cpp_keyword(self, name):
        """Escape C/C++ keywords by appending an underscore."""
        cpp_keywords = {
            'alignas', 'alignof', 'and', 'and_eq', 'asm', 'auto',
            'bitand', 'bitor', 'bool', 'break', 'case', 'catch',
            'char', 'char8_t', 'char16_t', 'char32_t', 'class', 'compl',
            'concept', 'const', 'consteval', 'constexpr', 'constinit',
            'const_cast', 'continue', 'co_await', 'co_return', 'co_yield',
            'decltype', 'default', 'delete', 'do', 'double', 'dynamic_cast',
            'else', 'enum', 'explicit', 'export', 'extern', 'false',
            'float', 'for', 'friend', 'goto', 'if', 'inline', 'int',
            'link',  # Special case for Windows link.exe conflicts
            'long', 'mutable', 'namespace', 'new', 'noexcept', 'not',
            'not_eq', 'nullptr', 'operator', 'or', 'or_eq', 'private',
            'protected', 'public', 'register', 'reinterpret_cast',
            'requires', 'return', 'short', 'signed', 'sizeof', 'static',
            'static_assert', 'static_cast', 'struct', 'switch',
            'template', 'this', 'thread_local', 'throw', 'true', 'try',
            'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using',
            'virtual', 'void', 'volatile', 'wchar_t', 'while', 'xor', 'xor_eq',
            # C keywords not in C++
            'restrict', '_Bool', '_Complex', '_Imaginary'
        }
        
        return f"{name}_" if name in cpp_keywords else name
    
    def _to_c_identifier(self, name):
        """Convert a ninja identifier into a valid C identifier.
        - Replace invalid characters with underscore
        - Ensure it starts with a letter or underscore
        - Deduplicate consecutive underscores
        - Escape C/C++ keywords
        """
        # First replace any invalid chars with underscore
        c_name = ''.join(c if c.isalnum() else '_' for c in name)
        
        # Ensure it starts with letter or underscore
        if c_name[0].isdigit():
            c_name = '_' + c_name
            
        # Collapse consecutive underscores
        while '__' in c_name:
            c_name = c_name.replace('__', '_')
        
        # Handle C/C++ keywords
        return self._escape_cpp_keyword(c_name)
    
    def generate_pool_declarations(self):
        """Generate C code for pool declarations."""
        lines = []
        for name, info in self.pools.items():
            c_name = self._to_c_identifier(name)
            lines.append(f"POOL({c_name})")
            if info['depth'] is not None:
                lines.append(f"  .depth = {info['depth']},")
            lines.append("END_POOL")
            lines.append("")
        return "\n".join(lines)
    
    def _escape_c_string(self, s):
        """Escape a string for use in C string literals."""
        return s.replace('\\', '\\\\').replace('"', '\\"')
    
    def _generate_eval_string(self, s):
        """Convert a string into EVAL format with LIT/VAR tokens without evaluating."""
        tokens = self._tokenize_string(s)
        parts = []
        for type_, value in tokens:
            if value.endswith('.d'):
                base_var = value[:-2]  # Remove the .d
                eval_str = f'VAR("{base_var}") LIT(".d")'
                parts.append(eval_str)
            else:
                escaped_value = self._escape_c_string(value)
                if type_ == 'L':
                    parts.append(f'LIT("{escaped_value}")')
                else:
                    parts.append(f'VAR("{escaped_value}")')
        return f"EVAL {' '.join(parts)} END"
    
    def _generate_binding(self, key, value, indent="    "):
        """Generate C code for a binding.
        Returns a string with the binding in either BL or full EVAL format.
        """
        if not value:  # Skip empty bindings
            return None
        
        tokens = self._tokenize_string(value)
        if len(tokens) == 1 and tokens[0][0] == 'L':
            escaped_value = self._escape_c_string(tokens[0][1])
            return f'{indent}BL({key}, "{escaped_value}")'
        else:
            eval_str = self._generate_eval_string(value)
            return f'{indent}{{"{key}", {eval_str}}},'
    
    def generate_bindings(self):
        """Generate C code for global bindings."""
        if not self.bindings:
            return ""
        
        lines = []
        lines.append("  BINDINGS")
        for key, value in self.bindings.items():
            binding = self._generate_binding(key, value)
            if binding:  # Only add non-None bindings
                lines.append(binding)
        lines.append("  END,")
        return "\n".join(lines)
    
    def _generate_paths_array(self, paths):
        """Generate a PATHS array of EvalStrings."""
        if not paths:
            return None
        
        lines = []
        lines.append("PATHS")
        for path in paths:
            eval_str = self._generate_eval_string(path)
            lines.append(f"    {eval_str},")
        lines.append("END")
        return "\n".join(lines)
    
    def generate_defaults(self):
        """Generate C code for default targets."""
        if not self.defaults:
            return ""
        
        paths = self._generate_paths_array(self.defaults)
        if paths:
            return f"  .defaults = {paths},"
        return ""
    
    def generate_edges(self):
        """Generate C code for build edges."""
        if not self.edges:
            return ""
        
        lines = []
        lines.append("  EDGES")
        for edge in self.edges:
            lines.append("    {")
            
            # Rule reference
            rule_name = edge['rule']
            if rule_name == 'phony':
                lines.append("      .rule = &PHONY_RULE,")
            else:
                c_rule_name = self._to_c_identifier(rule_name)
                lines.append(f"      .rule = &{c_rule_name},")
            
            # Pool reference
            if edge['pool'] == '':
                lines.append("      .pool = &DEFAULT_POOL,")
            elif edge['pool'] == 'console':
                lines.append("      .pool = &CONSOLE_POOL,")
            elif edge['pool']:
                c_pool_name = self._to_c_identifier(edge['pool'])
                lines.append(f"      .pool = &{c_pool_name},")
            
            # Input paths
            if edge['inputs']:
                paths = self._generate_paths_array(edge['inputs'])
                lines.append(f"      .in = {paths},")
            
            if edge['implicit_inputs']:
                paths = self._generate_paths_array(edge['implicit_inputs'])
                lines.append(f"      .implicit_deps = {paths},")
            
            if edge['order_only']:
                paths = self._generate_paths_array(edge['order_only'])
                lines.append(f"      .order_only_deps = {paths},")
            
            # Output paths
            if edge['outputs']:
                paths = self._generate_paths_array(edge['outputs'])
                lines.append(f"      .out = {paths},")
            
            if edge['implicit_outputs']:
                paths = self._generate_paths_array(edge['implicit_outputs'])
                lines.append(f"      .implicit_outs = {paths},")
            
            if edge['validation_inputs']:
                paths = self._generate_paths_array(edge['validation_inputs'])
                lines.append(f"      .validations = {paths},")
            
            # Bindings
            if edge['bindings']:
                lines.append("      BINDINGS")
                for key, value in edge['bindings'].items():
                    lines.append(self._generate_binding(key, value, indent="        "))
                lines.append("      END")
            
            lines.append("    },")  # Close the edge
        lines.append("  END,")  # Close the edges section
        return "\n".join(lines)
    
    def generate_rule_declarations(self):
        """Generate C code for rule declarations."""
        lines = []
        for name, bindings in self.rules.items():
            c_name = self._to_c_identifier(name)
            lines.append(f"RULE({c_name})")
            
            # Handle pool if present
            pool_name = bindings.get('pool')
            if pool_name:
                c_pool_name = self._to_c_identifier(pool_name)
                lines.append(f"  .pool = &{c_pool_name},")
                bindings = bindings.copy()
                del bindings['pool']
            
            if bindings:
                lines.append("  BINDINGS")
                for key, value in bindings.items():
                    lines.append(self._generate_binding(key, value))
                lines.append("  END")
            lines.append("END_RULE")  # Close the rule
            lines.append("")  # Add a blank line for separation
        return "\n".join(lines)
    
    def generate_c_code(self):
        """Generate complete C code representation of the manifest."""
        parts = [
            '#include "manifest.h"\n',
            "// Pool declarations",
            self.generate_pool_declarations(),
            "\n// Rule declarations",
            self.generate_rule_declarations(),
            "\n// Main manifest",
            "MANIFEST = {",
            self.generate_bindings(),
            self.generate_edges(),
            self.generate_defaults(),
            "};",
        ]
        return "\n".join(filter(None, parts))

if __name__ == '__main__':
    import sys
    import os
    import shutil
    import argparse
    
    # Create argument parser
    parser = argparse.ArgumentParser(description='Convert ninja build files to C')
    parser.add_argument('input', help='Input ninja file')
    parser.add_argument('output', nargs='?', help='Output C file (optional)')
    parser.add_argument('-d', '--display', action='store_true', 
                       help='Display parsed manifest structure')
    
    args = parser.parse_args()
    
    # Parse input file
    manifest = Manifest()
    try:
        manifest.parse_file(args.input)
        
        # If display flag is set, show the parsed structure
        if args.display:
            manifest.display_manifest()
            if not args.output:
                sys.exit(0)
        
        # Handle output file
        if args.output:
            output_file = args.output
        else:
            base = os.path.splitext(args.input)[0]
            output_file = f"{base}_ninja.c"
        
        # Generate and write C code
        c_code = manifest.generate_c_code()
        with open(output_file, 'w') as f:
            f.write(c_code)
        
        # Copy manifest.h to the output directory
        script_dir = os.path.dirname(os.path.abspath(__file__))
        manifest_h = os.path.join(script_dir, 'manifest.h')
        output_dir = os.path.dirname(os.path.abspath(output_file))
        shutil.copy2(manifest_h, output_dir)
        
        print(f"Generated {output_file}")
        print(f"Copied manifest.h to {output_dir}")
        
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)