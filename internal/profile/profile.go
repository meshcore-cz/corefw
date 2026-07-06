// Package profile models a corefw build profile: the declarative YAML document
// that composes a firmware image from a board, modules, policies and external
// component sources. It also implements ${var} substitution so a single
// parameterised profile can describe many similar devices.
package profile

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// Profile is a complete, top-level build composition.
type Profile struct {
	// Name identifies the resulting firmware image.
	Name string `yaml:"name"`
	// Platform pins the corefw platform version, e.g. "corefw@0.1".
	Platform string `yaml:"platform"`

	// Variables are user substitutions available as ${name} elsewhere.
	Variables map[string]any `yaml:"variables"`

	// External declares component sources fetched from git or local paths.
	External []ExternalSource `yaml:"external_components"`

	// Board is the target board component id.
	Board string `yaml:"board"`

	// Modules are device roles/services (repeater, companion, ...).
	Modules []ComponentRef `yaml:"modules"`

	// Policies tune shared mechanisms. Keyed by policy category (power,
	// routing, advert, ...); each value selects a policy component + options.
	Policies map[string]ComponentRef `yaml:"policies"`

	// Radio configures the regional/RF parameters (region, freq, bw, sf).
	Radio map[string]any `yaml:"radio"`

	// Features are coarse on/off toggles surfaced to end users.
	Features map[string]bool `yaml:"features"`
}

// ExternalSource declares where to fetch external components from.
type ExternalSource struct {
	Source     Source   `yaml:"source"`
	Components []string `yaml:"components"`
}

// Source is a git or local component source. It supports both the verbose form
// (a mapping with type/url/ref/path) and the shorthand string form
// "github://owner/repo@ref".
type Source struct {
	Type string `yaml:"type"` // "git" | "local"
	URL  string `yaml:"url"`
	Ref  string `yaml:"ref"` // branch, tag or commit
	Path string `yaml:"path"`
}

// UnmarshalYAML accepts either a shorthand scalar or the verbose mapping.
func (s *Source) UnmarshalYAML(node *yaml.Node) error {
	if node.Kind == yaml.ScalarNode {
		return s.parseShorthand(node.Value)
	}
	type raw Source
	var r raw
	if err := node.Decode(&r); err != nil {
		return err
	}
	*s = Source(r)
	if s.Type == "" {
		if s.Path != "" {
			s.Type = "local"
		} else {
			s.Type = "git"
		}
	}
	return nil
}

// ComponentRef references a component with optional inline options. It accepts:
//
//	- a scalar id:            "repeater"
//	- a mapping with type:    {type: repeater, admin_password: "x"}
//	- a single-key mapping:   {repeater: {admin_password: "x"}}
type ComponentRef struct {
	ID      string
	Options map[string]any
}

func (c *ComponentRef) UnmarshalYAML(node *yaml.Node) error {
	switch node.Kind {
	case yaml.ScalarNode:
		c.ID = node.Value
		c.Options = map[string]any{}
		return nil
	case yaml.MappingNode:
		var m map[string]any
		if err := node.Decode(&m); err != nil {
			return err
		}
		if t, ok := m["type"].(string); ok {
			c.ID = t
			delete(m, "type")
			c.Options = m
			return nil
		}
		if len(m) == 1 {
			for k, v := range m {
				c.ID = k
				if opts, ok := v.(map[string]any); ok {
					c.Options = opts
				} else {
					c.Options = map[string]any{}
				}
			}
			return nil
		}
		return fmt.Errorf("component ref must have a 'type' key or be a single-key mapping, got keys %v", keysOf(m))
	default:
		return fmt.Errorf("component ref must be a string or mapping")
	}
}

// Load reads, parses and substitutes a profile from disk.
func Load(path string) (*Profile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("reading profile: %w", err)
	}
	return Parse(data)
}

// Parse parses profile bytes and applies ${var} substitution using the
// document's own `variables:` block.
func Parse(data []byte) (*Profile, error) {
	// First pass: extract variables so we can substitute before full decode.
	var head struct {
		Variables map[string]any `yaml:"variables"`
	}
	if err := yaml.Unmarshal(data, &head); err != nil {
		return nil, fmt.Errorf("parsing profile variables: %w", err)
	}
	substituted, err := substitute(string(data), head.Variables)
	if err != nil {
		return nil, err
	}
	var p Profile
	if err := yaml.Unmarshal([]byte(substituted), &p); err != nil {
		return nil, fmt.Errorf("parsing profile: %w", err)
	}
	if p.Features == nil {
		p.Features = map[string]bool{}
	}
	return &p, nil
}

func keysOf(m map[string]any) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	return ks
}
