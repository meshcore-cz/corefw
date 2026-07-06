// Package registry discovers and loads components. It merges the built-in
// (embedded) components with external components fetched from git or local
// paths, and looks them up by id during resolution. Every loaded component
// carries its provenance so builds can report and lock exact origins.
package registry

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"

	"gopkg.in/yaml.v3"

	"github.com/arnal/corefw/components"
	"github.com/arnal/corefw/internal/manifest"
	"github.com/arnal/corefw/internal/schema"
)

// Origin records where a component was loaded from, for provenance/locking.
type Origin struct {
	Kind     string // "builtin" | "local" | "git"
	Ref      string // requested ref (git) or path (local)
	URL      string // git url, if applicable
	Resolved string // resolved commit (git), filled in by the source layer
	Dir      string // on-disk directory of the component
}

// Component is a fully loaded component: its manifest, its option schema and
// where it came from.
type Component struct {
	Manifest manifest.Manifest
	Schema   schema.Schema
	Origin   Origin

	// fsys/rel locate the component's files uniformly, whether it was embedded
	// (built-in) or loaded from disk. Used to read auxiliary support files.
	fsys fs.FS
	rel  string
}

// ReadFile reads a file relative to the component's directory, transparently
// handling embedded (built-in) and on-disk (local/git) components.
func (c *Component) ReadFile(rel string) ([]byte, error) {
	return fs.ReadFile(c.fsys, join(c.rel, rel))
}

// ID returns the component id.
func (c *Component) ID() string { return c.Manifest.ID }

// Registry holds all components available to a build, keyed by id.
type Registry struct {
	byID map[string]*Component
}

// New creates a registry pre-populated with the built-in components.
func New() (*Registry, error) {
	r := &Registry{byID: map[string]*Component{}}
	if err := r.loadEmbedded(); err != nil {
		return nil, err
	}
	return r, nil
}

// Get returns a component by id, or false if not present.
func (r *Registry) Get(id string) (*Component, bool) {
	c, ok := r.byID[id]
	return c, ok
}

// All returns every registered component, sorted by id.
func (r *Registry) All() []*Component {
	out := make([]*Component, 0, len(r.byID))
	for _, c := range r.byID {
		out = append(out, c)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID() < out[j].ID() })
	return out
}

// Add registers a component, rejecting id collisions so an external source
// cannot silently shadow a built-in (or another external) component.
func (r *Registry) Add(c *Component) error {
	if existing, ok := r.byID[c.ID()]; ok {
		return fmt.Errorf("component id %q from %s conflicts with existing component from %s",
			c.ID(), describe(c.Origin), describe(existing.Origin))
	}
	r.byID[c.ID()] = c
	return nil
}

func (r *Registry) loadEmbedded() error {
	return fs.WalkDir(components.FS, ".", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || filepath.Base(path) != "component.yaml" {
			return nil
		}
		dir := filepath.Dir(path)
		comp, err := loadFromFS(components.FS, dir)
		if err != nil {
			return fmt.Errorf("loading built-in component %s: %w", dir, err)
		}
		comp.Origin = Origin{Kind: "builtin", Ref: "embedded", Dir: dir}
		return r.Add(comp)
	})
}

// LoadDir loads a single component from an on-disk directory (used for local
// and freshly-fetched git components).
func LoadDir(dir string, origin Origin) (*Component, error) {
	comp, err := loadFromFS(os.DirFS(dir), ".")
	if err != nil {
		return nil, fmt.Errorf("loading component %s: %w", dir, err)
	}
	origin.Dir = dir
	comp.Origin = origin
	return comp, nil
}

// loadFromFS reads component.yaml and (optional) schema.yaml from a directory
// inside any fs.FS.
func loadFromFS(fsys fs.FS, dir string) (*Component, error) {
	comp := &Component{fsys: fsys, rel: dir}

	mBytes, err := fs.ReadFile(fsys, join(dir, "component.yaml"))
	if err != nil {
		return nil, err
	}
	if err := yaml.Unmarshal(mBytes, &comp.Manifest); err != nil {
		return nil, fmt.Errorf("parsing component.yaml: %w", err)
	}
	if comp.Manifest.ID == "" {
		return nil, fmt.Errorf("component.yaml missing id")
	}
	if comp.Manifest.Type == "" {
		return nil, fmt.Errorf("component %q missing type", comp.Manifest.ID)
	}

	if sBytes, err := fs.ReadFile(fsys, join(dir, "schema.yaml")); err == nil {
		if err := yaml.Unmarshal(sBytes, &comp.Schema); err != nil {
			return nil, fmt.Errorf("parsing schema.yaml: %w", err)
		}
	} else if !os.IsNotExist(err) && !isFSNotExist(err) {
		return nil, err
	}
	return comp, nil
}

func join(dir, name string) string {
	if dir == "." || dir == "" {
		return name
	}
	return dir + "/" + name
}

func isFSNotExist(err error) bool {
	return err != nil && (os.IsNotExist(err) || err.Error() == "file does not exist")
}

func describe(o Origin) string {
	switch o.Kind {
	case "git":
		return fmt.Sprintf("git %s@%s", o.URL, o.Ref)
	case "local":
		return fmt.Sprintf("local %s", o.Ref)
	default:
		return "built-in"
	}
}
