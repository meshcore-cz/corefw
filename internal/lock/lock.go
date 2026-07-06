// Package lock writes corefw.lock and build-manifest.json. Git-loaded
// components make builds powerful but potentially non-reproducible when refs
// move; the lockfile records the exact resolved commit of every external
// source and a hash of the generated configuration, so it is always possible to
// determine exactly what code a node is running.
package lock

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"

	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
)

// PlatformVersion is the corefw platform version stamped into lockfiles.
const PlatformVersion = "0.1.0"

// Lockfile is the reproducibility record for a build.
type Lockfile struct {
	Platform   string             `json:"platform"`
	Profile    string             `json:"profile"`
	Board      string             `json:"board"`
	ConfigHash string             `json:"config_hash"`
	Components []ComponentLock    `json:"components"`
	Sources    map[string]Source  `json:"sources"`
}

// ComponentLock records one selected component and its origin.
type ComponentLock struct {
	ID      string `json:"id"`
	Type    string `json:"type"`
	Version string `json:"version"`
	Status  string `json:"status"`
	Origin  string `json:"origin"`
}

// Source records an external git source and its resolved commit.
type Source struct {
	Requested string `json:"requested"`
	Resolved  string `json:"resolved_commit"`
}

// Build assembles a Lockfile from a resolved plan and the config hash.
func Build(plan *resolve.Plan) *Lockfile {
	lf := &Lockfile{
		Platform:   PlatformVersion,
		Profile:    plan.Profile.Name,
		Board:      plan.Board.ID(),
		ConfigHash: ConfigHash(plan),
		Sources:    map[string]Source{},
	}
	add := func(c *registry.Component) {
		lf.Components = append(lf.Components, ComponentLock{
			ID:      c.Manifest.ID,
			Type:    string(c.Manifest.Type),
			Version: c.Manifest.Version,
			Status:  string(c.Manifest.Status),
			Origin:  c.Origin.Kind,
		})
		if c.Origin.Kind == "git" {
			lf.Sources[c.Origin.URL] = Source{Requested: c.Origin.Ref, Resolved: c.Origin.Resolved}
		}
	}
	add(plan.Board.Component)
	for _, m := range plan.Modules {
		add(m.Component)
	}
	for _, p := range plan.Policies {
		add(p.Component)
	}
	sort.Slice(lf.Components, func(i, j int) bool { return lf.Components[i].ID < lf.Components[j].ID })
	return lf
}

// ConfigHash is a stable digest of the resolved selection + options, so any
// change to the effective configuration is detectable.
func ConfigHash(plan *resolve.Plan) string {
	h := sha256.New()
	enc := json.NewEncoder(h)
	type entry struct {
		ID       string
		Options  map[string]any
		Category string
	}
	all := []entry{{ID: plan.Board.ID(), Options: plan.Board.Options}}
	for _, m := range plan.Modules {
		all = append(all, entry{ID: m.ID(), Options: m.Options})
	}
	for _, p := range plan.Policies {
		all = append(all, entry{ID: p.ID(), Options: p.Options, Category: p.Category})
	}
	sort.Slice(all, func(i, j int) bool { return all[i].ID < all[j].ID })
	_ = enc.Encode(all)
	_ = enc.Encode(plan.Radio)
	return hex.EncodeToString(h.Sum(nil))
}

// WriteLock writes corefw.lock as pretty JSON.
func WriteLock(dir string, lf *Lockfile) (string, error) {
	path := filepath.Join(dir, "corefw.lock")
	return path, writeJSON(path, lf)
}

func writeJSON(path string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0o644)
}
