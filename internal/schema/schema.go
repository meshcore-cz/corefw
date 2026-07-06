// Package schema implements strict, declarative validation of component
// options. Each component ships a schema.yaml describing required/optional
// fields, types, ranges, units, defaults, enums and cross-field rules. Options
// are validated (and defaults applied) before any code generation happens, so
// mistakes surface as clear configuration errors instead of template
// instantiation failures deep inside a C++ compiler.
package schema

import (
	"fmt"
	"sort"
	"strings"
)

// Type enumerates the supported option value types.
type Type string

const (
	TypeString   Type = "string"
	TypeInteger  Type = "integer"
	TypeNumber   Type = "number"
	TypeBool     Type = "boolean"
	TypeDuration Type = "duration" // e.g. "15min", "1h", "30s"
	TypeEnum     Type = "enum"
)

// Property describes one option field.
type Property struct {
	Type       Type     `yaml:"type"`
	Default    any      `yaml:"default"`
	Minimum    *float64 `yaml:"minimum"`
	Maximum    *float64 `yaml:"maximum"`
	Enum       []string `yaml:"enum"`
	Unit       string   `yaml:"unit"`
	Deprecated string   `yaml:"deprecated"` // replacement key, if any
	Doc        string   `yaml:"description"`
}

// Rule is a simple cross-field constraint. Only "less_than" is needed for the
// MVP (e.g. critical_battery < low_battery), but the shape is extensible.
type Rule struct {
	LessThan   [2]string `yaml:"less_than"`   // [a, b] => a < b
	Message    string    `yaml:"message"`
}

// Schema is a component's option schema.
type Schema struct {
	Properties map[string]Property `yaml:"properties"`
	Required   []string            `yaml:"required"`
	Rules      []Rule              `yaml:"rules"`
}

// Validate checks the supplied options against the schema, filling in defaults
// for any missing optional fields. It returns a normalised option map (numbers
// coerced, defaults applied) or a descriptive error. The returned map is a new
// map; the input is not mutated.
func (s *Schema) Validate(component string, in map[string]any) (map[string]any, error) {
	out := make(map[string]any, len(s.Properties))

	// Reject unknown keys early — typos should fail, not be silently ignored.
	known := make(map[string]bool, len(s.Properties))
	for name := range s.Properties {
		known[name] = true
	}
	for _, key := range sortedKeys(in) {
		if !known[key] {
			return nil, fmt.Errorf("%s: unknown option %q (valid: %s)",
				component, key, strings.Join(sortedKeys(propMap(s.Properties)), ", "))
		}
	}

	for _, name := range sortedKeys(propMap(s.Properties)) {
		prop := s.Properties[name]
		raw, present := in[name]
		if !present {
			if prop.Default != nil {
				out[name] = prop.Default
			}
			continue
		}
		if prop.Deprecated != "" {
			// Accept but warn via error only if used with no replacement set.
			// (Warnings are surfaced by the caller; here we still validate.)
		}
		v, err := coerce(component, name, prop, raw)
		if err != nil {
			return nil, err
		}
		out[name] = v
	}

	// Required fields must be present after defaults are applied.
	for _, req := range s.Required {
		if _, ok := out[req]; !ok {
			return nil, fmt.Errorf("%s: required option %q is missing", component, req)
		}
	}

	if err := s.checkRules(component, out); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Schema) checkRules(component string, opts map[string]any) error {
	for _, r := range s.Rules {
		if r.LessThan[0] != "" {
			a, aok := asFloat(opts[r.LessThan[0]])
			b, bok := asFloat(opts[r.LessThan[1]])
			if aok && bok && !(a < b) {
				msg := r.Message
				if msg == "" {
					msg = fmt.Sprintf("%s must be lower than %s", r.LessThan[0], r.LessThan[1])
				}
				return fmt.Errorf("%s: %s\n\n    %s: %v\n    %s: %v",
					component, msg, r.LessThan[0], opts[r.LessThan[0]], r.LessThan[1], opts[r.LessThan[1]])
			}
		}
	}
	return nil
}

func coerce(component, name string, prop Property, raw any) (any, error) {
	switch prop.Type {
	case TypeString:
		s, ok := raw.(string)
		if !ok {
			return nil, typeErr(component, name, "string", raw)
		}
		return s, nil
	case TypeBool:
		b, ok := raw.(bool)
		if !ok {
			return nil, typeErr(component, name, "boolean", raw)
		}
		return b, nil
	case TypeInteger:
		f, ok := asFloat(raw)
		if !ok || f != float64(int64(f)) {
			return nil, typeErr(component, name, "integer", raw)
		}
		if err := checkRange(component, name, prop, f); err != nil {
			return nil, err
		}
		return int64(f), nil
	case TypeNumber:
		f, ok := asFloat(raw)
		if !ok {
			return nil, typeErr(component, name, "number", raw)
		}
		if err := checkRange(component, name, prop, f); err != nil {
			return nil, err
		}
		return f, nil
	case TypeEnum:
		s, ok := raw.(string)
		if !ok {
			return nil, typeErr(component, name, "enum string", raw)
		}
		for _, e := range prop.Enum {
			if e == s {
				return s, nil
			}
		}
		return nil, fmt.Errorf("%s: option %q must be one of [%s], got %q",
			component, name, strings.Join(prop.Enum, ", "), s)
	case TypeDuration:
		s, ok := raw.(string)
		if !ok {
			return nil, typeErr(component, name, "duration string (e.g. 15min)", raw)
		}
		if _, err := ParseDuration(s); err != nil {
			return nil, fmt.Errorf("%s: option %q: %v", component, name, err)
		}
		return s, nil
	default:
		return nil, fmt.Errorf("%s: option %q has unsupported schema type %q", component, name, prop.Type)
	}
}

func checkRange(component, name string, prop Property, f float64) error {
	if prop.Minimum != nil && f < *prop.Minimum {
		return fmt.Errorf("%s: option %q must be >= %v, got %v", component, name, *prop.Minimum, f)
	}
	if prop.Maximum != nil && f > *prop.Maximum {
		return fmt.Errorf("%s: option %q must be <= %v, got %v", component, name, *prop.Maximum, f)
	}
	return nil
}

func typeErr(component, name, want string, raw any) error {
	return fmt.Errorf("%s: option %q must be a %s, got %T (%v)", component, name, want, raw, raw)
}

func asFloat(v any) (float64, bool) {
	switch n := v.(type) {
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case float64:
		return n, true
	default:
		return 0, false
	}
}

func propMap(m map[string]Property) map[string]any {
	out := make(map[string]any, len(m))
	for k := range m {
		out[k] = struct{}{}
	}
	return out
}

func sortedKeys(m map[string]any) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
