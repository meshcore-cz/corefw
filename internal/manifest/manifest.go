// Package manifest defines the machine-readable description that every corefw
// component ships. A manifest separates a component's metadata, compatibility
// and dependency information from its C++ implementation, so the build system
// can reason about a component without compiling it.
package manifest

// Kind enumerates the component categories. They deliberately differ because
// each category has different privileges and responsibilities (see the
// architecture notes): a board assembles drivers and owns pins, a module
// provides a device role/service, a policy tunes shared mechanisms, a driver
// implements hardware, and an app consumes stable services.
type Kind string

const (
	KindBoard  Kind = "board"
	KindModule Kind = "module"
	KindPolicy Kind = "policy"
	KindDriver Kind = "driver"
	KindApp    Kind = "app"
)

// Status describes the maintenance state of a component. The platform team is
// not automatically responsible for community components.
type Status string

const (
	StatusOfficial   Status = "official"
	StatusVendor     Status = "vendor-maintained"
	StatusCommunity  Status = "community-maintained"
	StatusExperiment Status = "experimental"
	StatusDeprecated Status = "deprecated"
)

// Manifest is the parsed form of a component.yaml file.
type Manifest struct {
	ID          string   `yaml:"id"`
	Type        Kind     `yaml:"type"`
	Version     string   `yaml:"version"`
	Status      Status   `yaml:"status"`
	Maintainers []string `yaml:"maintainers"`
	License     string   `yaml:"license"`
	Description string   `yaml:"description"`

	// Compatibility pins the component to platform / API version ranges using
	// semver-style constraints (e.g. ">=0.1 <0.2").
	Compatibility Compatibility `yaml:"compatibility"`

	// Requires / Optional / Conflicts / AutoLoad drive dependency resolution.
	Requires  Requirements `yaml:"requires"`
	Optional  Requirements `yaml:"optional"`
	Conflicts []string     `yaml:"conflicts"`
	AutoLoad  []string     `yaml:"auto_load"`

	// Resources are approximate footprint estimates used for early budgeting.
	Resources Resources `yaml:"resources"`

	// Board carries board-only fields (platform, pins, capabilities, limits).
	Board *BoardSpec `yaml:"board,omitempty"`

	// Build describes how selecting this component affects the generated
	// PlatformIO project (defines, sources, libs, includes).
	Build Build `yaml:"build"`

	// Provides lists abstract services this component contributes (e.g. a
	// board "provides: [display]"). Modules/policies can require these.
	Provides []string `yaml:"provides"`

	// Codegen describes how to instantiate and register this component in the
	// generated firmware entrypoint. It is the bridge between declarative
	// configuration and the C++ kernel's registration API.
	Codegen Codegen `yaml:"codegen"`
}

// Codegen describes the C++ object the generator constructs for a component and
// how the component's validated options map onto setter calls.
type Codegen struct {
	// Class is the C++ type to instantiate (e.g. "HeltecV3Board").
	Class string `yaml:"class"`
	// Header is the include path for Class.
	Header string `yaml:"header"`
	// Var is the generated variable name; defaults to a slug of the id.
	Var string `yaml:"var"`
	// Register is the kernel API call used to register the instance, e.g.
	// "registerBoard", "registerModule", "setPowerPolicy".
	Register string `yaml:"register"`
	// Setters maps an option key to a C++ setter method. The validated option
	// value is passed as the argument (quoted for strings).
	Setters map[string]string `yaml:"setters"`
}

// Compatibility holds version constraints against the platform and its APIs.
type Compatibility struct {
	Platform  string `yaml:"platform"`
	BoardAPI  string `yaml:"board_api"`
	ModuleAPI string `yaml:"module_api"`
	PolicyAPI string `yaml:"policy_api"`
	RadioAPI  string `yaml:"radio_api"`
}

// Requirements groups dependency edges by target category.
type Requirements struct {
	Components   []string `yaml:"components"`
	Capabilities []string `yaml:"capabilities"`
	Services     []string `yaml:"services"`
}

// Resources are approximate, host-side estimates. The linker remains the final
// authority; these only improve error messages before a compile is attempted.
type Resources struct {
	Flash     int `yaml:"flash"`
	RAMStatic int `yaml:"ram_static"`
	Heap      int `yaml:"heap_expected"`
	Storage   int `yaml:"storage"`
}

// BoardSpec is the board-specific portion of a manifest.
type BoardSpec struct {
	Architecture string          `yaml:"architecture"` // esp32-s3, nrf52840
	Framework    string          `yaml:"framework"`    // arduino, esp-idf
	PlatformIO   PlatformIOBoard `yaml:"platformio"`
	Capabilities map[string]bool `yaml:"capabilities"`
	Limits       BoardLimits     `yaml:"limits"`
	// Display is the concrete display class + source a board offers, so modules
	// can request "display" without knowing which controller a board uses.
	Display *DisplaySpec `yaml:"display,omitempty"`
}

// PlatformIOBoard captures the target-specific bits the generator needs to emit
// a self-contained, buildable env against the C++ kernel.
type PlatformIOBoard struct {
	Board           string   `yaml:"board"`            // pio board id
	Platform        string   `yaml:"platform"`         // e.g. platformio/espressif32@6.11.0
	Framework       string   `yaml:"framework"`        // arduino / espidf
	PlatformPackages []string `yaml:"platform_packages"` // optional framework overrides
	BaseEnv         string   `yaml:"base_env"`         // legacy grouping label (esp32/nrf52)
	LdScript        string   `yaml:"ldscript"`         // optional linker script (project-relative)
	MaxSize         int      `yaml:"max_size"`         // optional upload size cap
	// SupportFiles are auxiliary files (custom board JSON, linker scripts) the
	// component ships that must be copied into the generated project's boards/
	// directory so PlatformIO can find them. Paths are relative to the
	// component directory.
	SupportFiles []string `yaml:"support_files"`
	// VariantFiles are the Arduino "variant" pin-definition sources (variant.h,
	// variant.cpp) some cores (nRF52) require. They are copied into a variant/
	// directory that is added to the include path and compiled.
	VariantFiles []string `yaml:"variant_files"`
}

// BoardLimits records hard ceilings the resolver enforces.
type BoardLimits struct {
	MaxTxPowerDBM int `yaml:"max_tx_power_dbm"`
	RAMBudget     int `yaml:"ram_budget"` // bytes; 0 = unknown
	FlashBudget   int `yaml:"flash_budget"`
}

// DisplaySpec names a display class and the sources needed to build it.
type DisplaySpec struct {
	Class string `yaml:"class"`
	// Header is the include path for Class (e.g.
	// "drivers/display/sh1106/SH1106Display.h"), emitted as -D DISPLAY_HEADER so
	// the shared platform main can #include it without naming a controller.
	Header  string   `yaml:"header"`
	Sources []string `yaml:"sources"`
}

// Build is a fragment describing this component's contribution to the build.
// Fragments from board + modules + policies are merged by the generator.
type Build struct {
	Defines   map[string]string `yaml:"defines"`
	SrcFilter []string          `yaml:"src_filter"`
	LibDeps   []string          `yaml:"lib_deps"`
	Includes  []string          `yaml:"includes"`
	// UsesDisplay, when true on a module, pulls in the active board's display
	// class + sources during generation.
	UsesDisplay bool `yaml:"uses_display"`
	// DefineTemplates map an option key to a C-preprocessor define whose value
	// comes from the resolved option value (see codegen).
	DefineTemplates map[string]string `yaml:"define_templates"`
}
