// Package source fetches external components declared in a profile and adds
// them to the registry. Local sources are read in place; git sources are cloned
// into a cache and pinned to an exact resolved commit so builds are
// reproducible. Fetching third-party components means compiling third-party
// native code, so provenance is always recorded and surfaced.
package source

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/registry"
)

// Fetcher resolves external sources into on-disk component directories.
type Fetcher struct {
	// CacheDir is where git sources are cloned. Defaults to ~/.cache/corefw.
	CacheDir string
	// BaseDir is the directory local relative paths resolve against (the
	// profile's directory).
	BaseDir string
	// Logf receives human-readable progress lines; may be nil.
	Logf func(format string, args ...any)
}

func (f *Fetcher) logf(format string, args ...any) {
	if f.Logf != nil {
		f.Logf(format, args...)
	}
}

// Resolve loads every requested component from the given external sources into
// the registry. It returns the loaded components (for lockfile recording).
func (f *Fetcher) Resolve(reg *registry.Registry, sources []profile.ExternalSource) ([]*registry.Component, error) {
	var loaded []*registry.Component
	for _, es := range sources {
		dir, origin, err := f.materialise(es.Source)
		if err != nil {
			return nil, err
		}
		comps, err := f.selectComponents(dir, origin, es.Components)
		if err != nil {
			return nil, err
		}
		for _, c := range comps {
			f.logf("  + %s %s (%s)", c.Manifest.Type, c.ID(), describeOrigin(c.Origin))
			if err := reg.Add(c); err != nil {
				return nil, err
			}
			loaded = append(loaded, c)
		}
	}
	return loaded, nil
}

// materialise returns a local directory containing the source's components,
// plus a populated Origin (with resolved commit for git).
func (f *Fetcher) materialise(s profile.Source) (string, registry.Origin, error) {
	switch s.Type {
	case "local":
		dir := s.Path
		if !filepath.IsAbs(dir) {
			dir = filepath.Join(f.BaseDir, dir)
		}
		if _, err := os.Stat(dir); err != nil {
			return "", registry.Origin{}, fmt.Errorf("local source %s: %w", dir, err)
		}
		return dir, registry.Origin{Kind: "local", Ref: s.Path}, nil
	case "git":
		return f.fetchGit(s)
	default:
		return "", registry.Origin{}, fmt.Errorf("unknown source type %q", s.Type)
	}
}

func (f *Fetcher) fetchGit(s profile.Source) (string, registry.Origin, error) {
	cache := f.CacheDir
	if cache == "" {
		home, _ := os.UserHomeDir()
		cache = filepath.Join(home, ".cache", "corefw", "sources")
	}
	dir := filepath.Join(cache, sanitise(s.URL))

	f.logf("  fetching %s@%s", s.URL, s.Ref)
	if _, err := os.Stat(filepath.Join(dir, ".git")); err != nil {
		if err := run("git", "clone", "--quiet", s.URL, dir); err != nil {
			return "", registry.Origin{}, fmt.Errorf("git clone %s: %w", s.URL, err)
		}
	} else {
		_ = run("git", "-C", dir, "fetch", "--quiet", "--all", "--tags")
	}
	// Fetch PR refs explicitly if requested (pull/N/head).
	if strings.HasPrefix(s.Ref, "pull/") {
		_ = run("git", "-C", dir, "fetch", "--quiet", "origin", s.Ref+":"+s.Ref)
	}
	if err := run("git", "-C", dir, "checkout", "--quiet", s.Ref); err != nil {
		return "", registry.Origin{}, fmt.Errorf("git checkout %s in %s: %w", s.Ref, s.URL, err)
	}
	commit, err := output("git", "-C", dir, "rev-parse", "HEAD")
	if err != nil {
		return "", registry.Origin{}, err
	}
	return dir, registry.Origin{
		Kind:     "git",
		URL:      s.URL,
		Ref:      s.Ref,
		Resolved: strings.TrimSpace(commit),
	}, nil
}

// selectComponents loads the named components from a source directory. If names
// is empty, every component found in the source is loaded. Components are
// searched for as directories containing a component.yaml (typically under
// components/, boards/, modules/ or policies/).
func (f *Fetcher) selectComponents(dir string, origin registry.Origin, names []string) ([]*registry.Component, error) {
	found := map[string]string{} // id -> dir
	err := filepath.WalkDir(dir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() && d.Name() == ".git" {
			return filepath.SkipDir
		}
		if d.IsDir() || d.Name() != "component.yaml" {
			return nil
		}
		cdir := filepath.Dir(path)
		c, err := registry.LoadDir(cdir, origin)
		if err != nil {
			return err
		}
		found[c.ID()] = cdir
		return nil
	})
	if err != nil {
		return nil, err
	}

	var out []*registry.Component
	if len(names) == 0 {
		for _, cdir := range found {
			c, err := registry.LoadDir(cdir, origin)
			if err != nil {
				return nil, err
			}
			out = append(out, c)
		}
		return out, nil
	}
	for _, name := range names {
		cdir, ok := found[name]
		if !ok {
			return nil, fmt.Errorf("component %q not found in source %s", name, describeOrigin(origin))
		}
		c, err := registry.LoadDir(cdir, origin)
		if err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, nil
}

func run(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func output(name string, args ...string) (string, error) {
	out, err := exec.Command(name, args...).Output()
	return string(out), err
}

func sanitise(url string) string {
	r := strings.NewReplacer("https://", "", "http://", "", "git@", "", "/", "_", ":", "_")
	return r.Replace(url)
}

func describeOrigin(o registry.Origin) string {
	switch o.Kind {
	case "git":
		short := o.Resolved
		if len(short) > 8 {
			short = short[:8]
		}
		return fmt.Sprintf("%s@%s", o.URL, short)
	case "local":
		return "local:" + o.Ref
	default:
		return "built-in"
	}
}
