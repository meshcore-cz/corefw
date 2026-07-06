// Package resolve turns a parsed profile plus a component registry into a
// validated build plan. It applies option schemas, pulls in auto-loaded
// dependencies, checks required capabilities/services, detects conflicts and
// runs cross-component final validation (tx-power limits, display availability,
// resource budgets). Individual components can be valid alone yet invalid
// together; this stage is where that is caught, with clear messages, before any
// code is generated.
package resolve

import (
	"fmt"
	"sort"
	"strings"

	"github.com/arnal/corefw/internal/manifest"
	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/registry"
)

// Selected is a component chosen for the build with its validated options.
type Selected struct {
	Component *registry.Component
	Options   map[string]any
	Category  string // policy category (power/routing/advert...); empty for modules
}

// ID is a convenience accessor.
func (s *Selected) ID() string { return s.Component.ID() }

// Plan is the fully-resolved, validated composition consumed by codegen.
type Plan struct {
	Profile      *profile.Profile
	Board        *Selected
	Modules      []*Selected
	Policies     []*Selected
	Radio        map[string]any
	Capabilities map[string]bool
	Warnings     []string
}

// Resolve builds and validates the plan.
func Resolve(p *profile.Profile, reg *registry.Registry) (*Plan, error) {
	plan := &Plan{Profile: p, Radio: p.Radio, Capabilities: map[string]bool{}}

	// --- Board -------------------------------------------------------------
	if p.Board == "" {
		return nil, fmt.Errorf("profile has no board")
	}
	boardComp, ok := reg.Get(p.Board)
	if !ok {
		return nil, fmt.Errorf("unknown board %q (run `corefw components` to list available)", p.Board)
	}
	if boardComp.Manifest.Type != manifest.KindBoard {
		return nil, fmt.Errorf("%q is a %s, not a board", p.Board, boardComp.Manifest.Type)
	}
	boardOpts, err := validateOptions(boardComp, nil)
	if err != nil {
		return nil, err
	}
	plan.Board = &Selected{Component: boardComp, Options: boardOpts}
	if b := boardComp.Manifest.Board; b != nil {
		for cap, on := range b.Capabilities {
			plan.Capabilities[cap] = on
		}
	}

	// Radio defaults: a profile may omit the radio block entirely (or set only
	// some keys) and inherit the board's defaults. Profile keys win per-key.
	plan.Radio = mergeRadioDefaults(boardComp.Manifest.Board, p.Radio)

	// Available "services" a dependent can require: board capabilities +
	// everything a selected component `provides`.
	provided := map[string]string{} // service -> providing component id
	for cap, on := range plan.Capabilities {
		if on {
			provided[cap] = plan.Board.ID()
		}
	}
	for _, prov := range boardComp.Manifest.Provides {
		provided[prov] = plan.Board.ID()
	}

	selectedIDs := map[string]bool{plan.Board.ID(): true}

	// --- Modules (with auto_load expansion) --------------------------------
	moduleRefs := expandAutoLoad(p.Modules, reg, &plan.Warnings)
	for _, ref := range moduleRefs {
		comp, ok := reg.Get(ref.ID)
		if !ok {
			return nil, fmt.Errorf("unknown module %q", ref.ID)
		}
		if comp.Manifest.Type != manifest.KindModule {
			return nil, fmt.Errorf("%q is a %s, cannot be listed under modules", ref.ID, comp.Manifest.Type)
		}
		opts, err := validateOptions(comp, ref.Options)
		if err != nil {
			return nil, err
		}
		sel := &Selected{Component: comp, Options: opts}
		plan.Modules = append(plan.Modules, sel)
		selectedIDs[sel.ID()] = true
		for _, prov := range comp.Manifest.Provides {
			provided[prov] = sel.ID()
		}
	}

	// --- Policies ----------------------------------------------------------
	// Effective policies = the board's default policies for each category the
	// profile leaves unset, overlaid by the profile's own policies. So a minimal
	// profile still gets (e.g.) a power policy with schema-default thresholds.
	effPolicies := effectivePolicies(boardComp.Manifest.Board, p.Policies)
	for _, category := range sortedPolicyKeys(effPolicies) {
		ref := effPolicies[category]
		comp, ok := reg.Get(ref.ID)
		if !ok {
			return nil, fmt.Errorf("unknown policy %q (category %q)", ref.ID, category)
		}
		if comp.Manifest.Type != manifest.KindPolicy {
			return nil, fmt.Errorf("%q is a %s, cannot be used as a policy", ref.ID, comp.Manifest.Type)
		}
		opts, err := validateOptions(comp, ref.Options)
		if err != nil {
			return nil, err
		}
		sel := &Selected{Component: comp, Options: opts, Category: category}
		plan.Policies = append(plan.Policies, sel)
		selectedIDs[sel.ID()] = true
	}

	// --- Dependency, conflict & capability checks --------------------------
	if err := checkDependencies(plan, provided, selectedIDs); err != nil {
		return nil, err
	}
	if err := checkConflicts(plan, selectedIDs); err != nil {
		return nil, err
	}
	if err := finalValidation(plan); err != nil {
		return nil, err
	}
	return plan, nil
}

// all returns board + modules + policies in a single slice for uniform checks.
func (p *Plan) all() []*Selected {
	out := []*Selected{p.Board}
	out = append(out, p.Modules...)
	out = append(out, p.Policies...)
	return out
}

func checkDependencies(plan *Plan, provided map[string]string, selected map[string]bool) error {
	for _, sel := range plan.all() {
		m := sel.Component.Manifest
		for _, cap := range m.Requires.Capabilities {
			if _, ok := provided[cap]; !ok {
				return fmt.Errorf("%s requires capability %q, but board %s does not provide it",
					sel.ID(), cap, plan.Board.ID())
			}
		}
		for _, svc := range m.Requires.Services {
			if _, ok := provided[svc]; !ok {
				return fmt.Errorf("%s requires service %q, which no selected component provides",
					sel.ID(), svc)
			}
		}
		for _, dep := range m.Requires.Components {
			if !selected[dep] {
				return fmt.Errorf("%s requires component %q, which is not selected", sel.ID(), dep)
			}
		}
	}
	return nil
}

func checkConflicts(plan *Plan, selected map[string]bool) error {
	for _, sel := range plan.all() {
		for _, c := range sel.Component.Manifest.Conflicts {
			if selected[c] {
				return fmt.Errorf("%s conflicts with %s; both are selected", sel.ID(), c)
			}
		}
	}
	// At most one policy per category is enforced structurally (map keys), so
	// no extra check is needed there.
	return nil
}

// finalValidation runs cross-component sanity checks that only make sense once
// the whole graph is known.
func finalValidation(plan *Plan) error {
	board := plan.Board.Component.Manifest.Board
	if board == nil {
		return fmt.Errorf("board %s has no board spec", plan.Board.ID())
	}

	// TX power must not exceed the board's hardware limit.
	if txp, ok := findTxPower(plan); ok && board.Limits.MaxTxPowerDBM > 0 && txp > board.Limits.MaxTxPowerDBM {
		return fmt.Errorf("configured tx_power %d dBm exceeds board %s limit of %d dBm",
			txp, plan.Board.ID(), board.Limits.MaxTxPowerDBM)
	}

	// A module that uses a display requires the board to offer one.
	for _, mod := range plan.Modules {
		if mod.Component.Manifest.Build.UsesDisplay {
			if board.Display == nil || !board.Capabilities["display"] {
				plan.Warnings = append(plan.Warnings, fmt.Sprintf(
					"module %s can use a display, but board %s provides none; building without display",
					mod.ID(), plan.Board.ID()))
			}
		}
	}

	// Early RAM budget estimate (linker remains the final authority).
	if board.Limits.RAMBudget > 0 {
		var ram int
		for _, sel := range plan.all() {
			ram += sel.Component.Manifest.Resources.RAMStatic
		}
		if ram > board.Limits.RAMBudget {
			return fmt.Errorf("estimated static RAM %d bytes exceeds board %s budget of %d bytes",
				ram, plan.Board.ID(), board.Limits.RAMBudget)
		}
	}
	return nil
}

// findTxPower looks for a tx_power option on any selected policy.
func findTxPower(plan *Plan) (int, bool) {
	for _, pol := range plan.Policies {
		if v, ok := pol.Options["tx_power"]; ok {
			if n, ok := toInt(v); ok {
				return n, true
			}
		}
	}
	return 0, false
}

// expandAutoLoad appends auto_load dependencies of each referenced module that
// are not already listed, preserving order and de-duplicating.
func expandAutoLoad(refs []profile.ComponentRef, reg *registry.Registry, warnings *[]string) []profile.ComponentRef {
	seen := map[string]bool{}
	var out []profile.ComponentRef
	add := func(r profile.ComponentRef) {
		if seen[r.ID] {
			return
		}
		seen[r.ID] = true
		out = append(out, r)
	}
	for _, r := range refs {
		if comp, ok := reg.Get(r.ID); ok {
			for _, dep := range comp.Manifest.AutoLoad {
				if !seen[dep] {
					add(profile.ComponentRef{ID: dep, Options: map[string]any{}})
					*warnings = append(*warnings, fmt.Sprintf("auto-loaded %q required by %q", dep, r.ID))
				}
			}
		}
		add(r)
	}
	return out
}

// mergeRadioDefaults overlays a profile's radio block onto the board's radio
// defaults (profile keys win). Returns nil when neither supplies anything.
func mergeRadioDefaults(board *manifest.BoardSpec, profileRadio map[string]any) map[string]any {
	out := map[string]any{}
	if board != nil {
		for k, v := range board.Defaults.Radio {
			out[k] = v
		}
	}
	for k, v := range profileRadio {
		out[k] = v
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

// effectivePolicies combines the board's default policies (one component id per
// category) with the profile's policies. A profile entry in a category fully
// replaces the board default for that category (options come from the profile).
func effectivePolicies(board *manifest.BoardSpec, profilePolicies map[string]profile.ComponentRef) map[string]profile.ComponentRef {
	out := map[string]profile.ComponentRef{}
	if board != nil {
		for category, id := range board.Defaults.Policies {
			out[category] = profile.ComponentRef{ID: id, Options: map[string]any{}}
		}
	}
	for category, ref := range profilePolicies {
		out[category] = ref
	}
	return out
}

func validateOptions(c *registry.Component, in map[string]any) (map[string]any, error) {
	if in == nil {
		in = map[string]any{}
	}
	if len(c.Schema.Properties) == 0 {
		// No schema: reject any options to avoid silent typos.
		if len(in) > 0 {
			return nil, fmt.Errorf("%s accepts no options, got %v", c.ID(), keys(in))
		}
		return map[string]any{}, nil
	}
	return c.Schema.Validate(c.ID(), in)
}

func sortedPolicyKeys(m map[string]profile.ComponentRef) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}

func toInt(v any) (int, bool) {
	switch n := v.(type) {
	case int:
		return n, true
	case int64:
		return int(n), true
	case float64:
		return int(n), true
	}
	return 0, false
}

func keys(m map[string]any) string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return strings.Join(ks, ", ")
}
