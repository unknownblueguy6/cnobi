class Manifest:
    """Stores the parsed contents of a build.ninja file."""
    
    def __init__(self):
        self.pools = {}    # name -> {'depth': N}
        self.rules = {}    # name -> {bindings}
        self.bindings = {} # name -> value
        self.edges = []    # list of build edges
        self.defaults = [] # list of default targets
        
    def parse_file(self, filepath):
        """Parse a .ninja file and populate the manifest."""
        with open(filepath, 'r', encoding='utf-8') as f:
            self._parse_contents(f, filepath)
    
    def _read_line(self, f):
        """Read a line from file, handling comments and line continuations."""
        line_continuation = []
        
        while True:
            line = f.readline()
            if not line:  # EOF
                return None
            
            # Handle comments
            comment_pos = line.find('#')
            if comment_pos >= 0:
                line = line[:comment_pos]
            
            # Handle line continuation
            if line_continuation:
                line = line.lstrip()  # Always strip leading whitespace after continuation
                if line.rstrip().endswith('$'):
                    line_continuation.append(line.rstrip()[:-1])  # remove $ but keep other trailing spaces
                    continue
                line_continuation.append(line.rstrip())
                line = ''.join(line_continuation)
                line_continuation = []
            elif line.rstrip().endswith('$'):
                line_continuation.append(line.rstrip()[:-1])  # remove $ but keep other trailing spaces
                continue
            else:
                line = line.rstrip()
            
            # Skip truly empty lines (no content after stripping)
            if not line and not line_continuation:
                continue
            
            return line
    
    def _parse_contents(self, f, filepath):
        """Parse the contents of a .ninja file line by line."""
        while True:
            line = self._read_line(f)
            if line is None:  # EOF
                break
            
            if line.startswith('pool '):
                self._parse_pool(f, line)
            elif line.startswith('rule '):
                self._parse_rule(f, line)
            elif line.startswith('build '):
                self._parse_edge(f, line)
            elif line.startswith('default '):
                parts = self._split_on_unescaped_spaces(line)
                self.defaults.extend(parts[1:])  # Skip 'default'
                continue  # Make sure we don't try to parse this as a binding
            elif line.startswith('include '):
                self._handle_include(line[8:].strip(), filepath)
            elif line.startswith('subninja '):
                raise ValueError("subninja is not supported")
            else:
                self._parse_binding(line, self.bindings)
    
    def _is_indented(self, line):
        """Return True if the line starts with any whitespace."""
        return line and (line[0].isspace())
    
    def _put_back_line(self, f, line):
        """Put back a line to be read again by seeking back in the file."""
        pos = f.tell() - len(line) - 1  # -1 for newline
        f.seek(pos)
    
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
                self._put_back_line(f, line)
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
                self._put_back_line(f, line)
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
        in_parts = self._split_on_unescaped_spaces(out_in[1])
        rule_name = in_parts[0]
        inputs = []
        implicit_inputs = []
        order_only = []
        validation_inputs = []
        current = inputs
        
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
                self._put_back_line(f, line)
                break
            line = line.strip()
            if line.startswith('pool = '):
                edge['pool'] = line.split('=')[1].strip()
            else:
                self._parse_binding(line, edge['bindings'])
        
        self.edges.append(edge)
    
    def _parse_binding(self, line, bindings_dict):
        """Parse a variable binding."""
        parts = line.split('=', 1)
        name = parts[0].strip()
        value = parts[1].strip() if len(parts) > 1 else ''
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
                    while i < len(s) and not s[i].isspace() and s[i] not in '$:':
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

if __name__ == '__main__':
    import sys
    
    if len(sys.argv) != 2:
        print("Usage: python manifest.py build.ninja")
        sys.exit(1)
        
    manifest = Manifest()
    manifest.parse_file(sys.argv[1])
    manifest.display_manifest()