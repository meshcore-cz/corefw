package codegen

import (
	"sort"

	"github.com/arnal/corefw/internal/resolve"
)

// MergedBuild is the union of every selected component's build fragment: the
// preprocessor defines, source filters, library dependencies and include paths
// that together describe the firmware image. Only selected components
// contribute, so unused drivers and apps are never compiled in.
type MergedBuild struct {
	Defines   map[string]string
	SrcFilter []string
	LibDeps   []string
	Includes  []string
}

// merge folds board + module + policy + radio fragments into one build. Order
// is deterministic (board first, then modules, then policies) so generated
// output is stable and diff-friendly.
func merge(plan *resolve.Plan) MergedBuild {
	mb := MergedBuild{Defines: map[string]string{}}
	seenSrc := map[string]bool{}
	seenLib := map[string]bool{}
	seenInc := map[string]bool{}

	addSrc := func(items []string) {
		for _, s := range items {
			if !seenSrc[s] {
				seenSrc[s] = true
				mb.SrcFilter = append(mb.SrcFilter, s)
			}
		}
	}
	addLib := func(items []string) {
		for _, s := range items {
			if !seenLib[s] {
				seenLib[s] = true
				mb.LibDeps = append(mb.LibDeps, s)
			}
		}
	}
	addInc := func(items []string) {
		for _, s := range items {
			if !seenInc[s] {
				seenInc[s] = true
				mb.Includes = append(mb.Includes, s)
			}
		}
	}

	board := plan.Board.Component.Manifest.Board
	apply := func(sel *resolve.Selected) {
		b := sel.Component.Manifest.Build
		for k, v := range b.Defines {
			mb.Defines[k] = v
		}
		// define_templates turn validated options into value-carrying defines.
		// Strings are emitted as quoted C string literals ('"..."').
		for optKey, defName := range b.DefineTemplates {
			if v, ok := sel.Options[optKey]; ok {
				if s, isStr := v.(string); isStr {
					mb.Defines[defName] = quote(s)
				} else {
					mb.Defines[defName] = defineValue(v)
				}
			}
		}
		addSrc(b.SrcFilter)
		addLib(b.LibDeps)
		addInc(b.Includes)
		// A module that uses a display pulls in the board's concrete display.
		if b.UsesDisplay && board != nil && board.Display != nil {
			mb.Defines["DISPLAY_CLASS"] = board.Display.Class
			addSrc(board.Display.Sources)
		}
	}

	apply(plan.Board)
	for _, m := range plan.Modules {
		apply(m)
	}
	for _, p := range plan.Policies {
		apply(p)
	}
	applyRadio(&mb, plan.Radio)

	return mb
}

// applyRadio maps the profile's radio block onto the kernel's LoRa defines,
// keeping corefw wire-compatible with meshcore's regional parameters.
func applyRadio(mb *MergedBuild, radio map[string]any) {
	if radio == nil {
		return
	}
	if v, ok := radio["freq"]; ok {
		mb.Defines["LORA_FREQ"] = defineValue(v)
	}
	if v, ok := radio["bandwidth"]; ok {
		mb.Defines["LORA_BW"] = defineValue(v)
	}
	if v, ok := radio["spreading_factor"]; ok {
		mb.Defines["LORA_SF"] = defineValue(v)
	}
	if v, ok := radio["region"]; ok {
		mb.Defines["LORA_REGION"] = quote(defineValue(v))
	}
}

// sortedDefines returns define keys in stable order for emission.
func (mb MergedBuild) sortedDefines() []string {
	ks := make([]string, 0, len(mb.Defines))
	for k := range mb.Defines {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}
