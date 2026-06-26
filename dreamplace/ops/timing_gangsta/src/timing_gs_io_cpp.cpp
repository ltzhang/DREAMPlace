#include "timing_gs_io_cpp.h"
#include <cstring>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cstdlib>
#include <sstream>
#include <limbo/programoptions/ProgramOptions.h>
#include "utility/src/utils.h"
#include "place_io/src/PlaceDB.h"

DREAMPLACE_BEGIN_NAMESPACE

// Global storage for NetlistDB data
TimingGangstaIO::NetlistDataStorage TimingGangstaIO::g_netlist_data;

// Pin permutation maps (seam C.4).
std::vector<int32_t> TimingGangstaIO::g2d_;
std::vector<int32_t> TimingGangstaIO::d2g_;

// Callback function for GangSTA logging integration
void dreamplace_gangsta_print_callback(uint8_t level, const char* message) {
	MessageType dreamplace_level;
	switch (level) {
		case 1: dreamplace_level = kERROR; break;
		case 2: dreamplace_level = kWARN; break;
		case 3: dreamplace_level = kINFO; break;
		case 4: dreamplace_level = kDEBUG; break;
		case 5: dreamplace_level = kDEBUG; break;
		default: dreamplace_level = kINFO; break;
	}
	dreamplacePrint(dreamplace_level, "%s\n", message);
}

void TimingGangstaIO::NetlistDataStorage::clear() {
	cell_names.clear();
	cell_types.clear();
	pin_names.clear();
	net_names.clear();
	cell_name_ptrs.clear();
	cell_type_ptrs.clear();
	pin_name_ptrs.clear();
	net_name_ptrs.clear();
	pin_directions.clear();
	pin2cell_map.clear();
	pin2net_map.clear();
	nets_zero.clear();
	nets_one.clear();
	design_name.clear();
}

/// @brief Parse all timing options from command line arguments
/// @param argc 
/// @param argv 
/// @param early_lib_paths early timing library paths
/// @param late_lib_paths late timing library paths
/// @param shared_lib_paths shared timing library paths
/// @param sdc_path  SDC file path
/// @return 
bool TimingGangstaIO::parse_all_timing_options(int argc, char** argv, 
		std::vector<std::string>& early_lib_paths, std::vector<std::string>& late_lib_paths,
		std::vector<std::string>& shared_lib_paths, std::string& sdc_path) {
	typedef limbo::programoptions::ProgramOptions po_type;
	using limbo::programoptions::Value;
	po_type desc(std::string("All GangSTA timing options"));

	desc.add_option(Value<std::vector<std::string>>("--early_lib_input", &early_lib_paths, "input celllib file(s) (early)"))
		.add_option(Value<std::vector<std::string>>("--late_lib_input", &late_lib_paths, "input celllib file(s) (late)"))
		.add_option(Value<std::vector<std::string>>("--lib_input", &shared_lib_paths, "input shared celllib file(s)"))
		.add_option(Value<std::string>("--sdc_input", &sdc_path, "input sdc file"));

	try {
		desc.parse(argc, argv);
		if (!shared_lib_paths.empty() && (!early_lib_paths.empty() || !late_lib_paths.empty())) {
			dreamplacePrint(kERROR, "lib_input cannot be used together with early_lib_input or late_lib_input\n");
			return false;
		}
		return true;
	} catch (std::exception& e) {
		dreamplacePrint(kERROR, "Error parsing timing arguments: %s\n", e.what());
		return false;
	}
}

namespace {

std::string join_lib_paths(const std::vector<std::string>& paths) {
	std::ostringstream oss;
	for (size_t i = 0; i < paths.size(); ++i) {
		if (i != 0) {
			oss << ", ";
		}
		oss << paths[i];
	}
	return oss.str();
}

bool read_liberty_group(GangstaTimer& sta, GangstaEarlyLate el, const std::vector<std::string>& lib_paths) {
	if (lib_paths.empty()) {
		return true;
	}

	const char* corner_name = (el == GANGSTA_EARLY) ? "early" : "late";
	if (lib_paths.size() == 1) {
		const std::string& lib_path = lib_paths.front();
		if (!gangsta_read_liberty(&sta, el, lib_path.c_str())) {
			dreamplacePrint(kERROR, "Failed to load %s Liberty library: %s\n", corner_name, lib_path.c_str());
			return false;
		}
		dreamplacePrint(kDEBUG, "%s Liberty library loaded: %s\n", corner_name, lib_path.c_str());
		return true;
	}

	std::vector<const char*> c_paths;
	c_paths.reserve(lib_paths.size());
	for (const std::string& path : lib_paths) {
		c_paths.push_back(path.c_str());
	}

	if (!gangsta_batch_read_liberty(&sta, el, c_paths.data(), c_paths.size())) {
		const std::string joined_paths = join_lib_paths(lib_paths);
		dreamplacePrint(kERROR, "Failed to load %s Liberty libraries (%zu files): %s\n",
				corner_name, lib_paths.size(), joined_paths.c_str());
		return false;
	}

	const std::string joined_paths = join_lib_paths(lib_paths);
	dreamplacePrint(kDEBUG, "%s Liberty libraries loaded (%zu files): %s\n",
			corner_name, lib_paths.size(), joined_paths.c_str());
	return true;
}

}  // namespace

/// @brief Read timing libraries from specified paths
/// @param sta 
/// @param early_lib_paths 
/// @param late_lib_paths 
/// @param shared_lib_paths 
/// @return 
bool TimingGangstaIO::read_liberty_libraries_with_paths(GangstaTimer& sta, 
		const std::vector<std::string>& early_lib_paths,
		const std::vector<std::string>& late_lib_paths, 
		const std::vector<std::string>& shared_lib_paths) {
	// Shared library files for both early and late
	if (!shared_lib_paths.empty()) {
		return read_liberty_group(sta, GANGSTA_EARLY, shared_lib_paths) &&
				read_liberty_group(sta, GANGSTA_LATE, shared_lib_paths);
	}

	// Separate early/late library files
	bool success = true;
	bool has_any_lib = !early_lib_paths.empty() || !late_lib_paths.empty();

	success = read_liberty_group(sta, GANGSTA_EARLY, early_lib_paths) && success;
	success = read_liberty_group(sta, GANGSTA_LATE, late_lib_paths) && success;

	if (!has_any_lib) {
		dreamplacePrint(kERROR, "No Liberty library specified\n");
		return false;
	}

	return success;
}

/// 
/// @brief Read SDC constraints from specified path
/// @param sta 
/// @param sdc_path 
/// @return 
bool TimingGangstaIO::read_sdc_constraints_with_path(GangstaTimer& sta, const std::string& sdc_path) {
	if (sdc_path.empty()) {
		dreamplacePrint(kWARN, "No SDC file specified - timing analysis will have no constraints\n");
		return true; 
	}

	dreamplacePrint(kINFO, "Reading SDC constraints from: %s\n", sdc_path.c_str());
	if (gangsta_read_sdc(&sta, sdc_path.c_str())) {
		dreamplacePrint(kDEBUG, "SDC file parsing completed successfully\n");
		return true;
	} else {
		dreamplacePrint(kERROR, "SDC parsing failed for file: %s\n", sdc_path.c_str());
		return false;
	}
}

/// 
/// @brief Setup timing engine with PlaceDB and DREAMPlace mappings
/// @param sta 
/// @param placedb 
/// @param argc 
/// @param argv 
/// @param dreamplace_mappings 
/// @return 
bool TimingGangstaIO::setupTiming(GangstaTimer& sta, PlaceDB& placedb, int argc, char** argv, 
		const pybind11::dict& dreamplace_mappings) {
	// Parse all timing-related arguments
	std::vector<std::string> early_lib_paths, late_lib_paths, shared_lib_paths;
	std::string sdc_path;
	if (!parse_all_timing_options(argc, argv, early_lib_paths, late_lib_paths, shared_lib_paths, sdc_path)) {
		dreamplacePrint(kERROR, "Failed to parse timing arguments\n");
		return false;
	}

	// Read Liberty libraries
	if (!read_liberty_libraries_with_paths(sta, early_lib_paths, late_lib_paths, shared_lib_paths)) {
		dreamplacePrint(kERROR, "Failed to read Liberty libraries\n");
		return false;
	}

	// Populate g_netlist_data from DREAMPlace mappings (GangSTA in-memory netlist; no NetlistDB).
	dreamplacePrint(kINFO, "Using DREAMPlace mappings to build the GangSTA in-memory netlist\n");
	if (!build_netlistdb_from_dreamplace(placedb, dreamplace_mappings)) {
		dreamplacePrint(kERROR, "Failed to assemble GangSTA netlist arrays from DREAMPlace mappings\n");
		return false;
	}

	// Setup timer database
	if (!buildTimerDB(sta)) {
		dreamplacePrint(kERROR, "Failed to build timer database\n");
		return false;
	}

	// Seam C.4: build the GangSTA<->DREAMPlace pin permutation now that the graph is built.
	if (!build_pin_maps(sta)) {
		dreamplacePrint(kWARN, "Some DREAMPlace pins did not map to GangSTA pins; timing may be partial\n");
	}

	// Read SDC constraints with enhanced error handling
	if (!read_sdc_constraints_with_path(sta, sdc_path)) {
		dreamplacePrint(kERROR, "SDC constraints reading failed for: %s\n", sdc_path.c_str());
	} else {
		dreamplacePrint(kINFO, "SDC constraints loaded successfully from: %s\n", sdc_path.c_str());
	}

	return true;
}
bool TimingGangstaIO::build_netlistdb_from_dreamplace(PlaceDB& placedb,
		const pybind11::dict& dreamplace_mappings) {
	dreamplacePrint(kINFO, "Assembling GangSTA in-memory netlist arrays from DREAMPlace mappings\n");

	// Clear previous data
	g_netlist_data.clear();

	try {
		// Extract only the DREAMPlace mappings that are actually used - must match _package_dreamplace_mappings() keys
		auto pin2net_map = dreamplace_mappings["pin2net_map"].cast<torch::Tensor>();
		auto pin2node_map = dreamplace_mappings["pin2node_map"].cast<torch::Tensor>();
		auto pin_direct = dreamplace_mappings["pin_direct"].cast<torch::Tensor>();


		// Convert torch tensors to CPU and get data pointers
		auto pin2net_cpu = pin2net_map.to(torch::kCPU).to(torch::kInt32);
		auto pin2node_cpu = pin2node_map.to(torch::kCPU).to(torch::kInt32);
		auto pin_direct_cpu = pin_direct.to(torch::kCPU).to(torch::kUInt8);

		const int32_t* pin2net_data = DREAMPLACE_TENSOR_DATA_PTR(pin2net_cpu, int32_t);
		const int32_t* pin2node_data = DREAMPLACE_TENSOR_DATA_PTR(pin2node_cpu, int32_t);
		const uint8_t* pin_direct_data = DREAMPLACE_TENSOR_DATA_PTR(pin_direct_cpu, uint8_t);

		size_t num_pins = pin2net_cpu.numel();
		size_t num_nodes = placedb.nodes().size();
		size_t num_nets = placedb.nets().size();

		// Extract num_terminal_NIs from dreamplace_mappings
		auto num_terminal_NIs_tensor = dreamplace_mappings["num_terminal_NIs"].cast<torch::Tensor>();
		size_t num_terminal_NIs = num_terminal_NIs_tensor.item<int32_t>();

		// Analyze PlaceDB structure for IO pin identification
		size_t numMovable, numFixed, numPlaceBlockages, iopinNodeStart;
		numMovable = placedb.numMovable();
		numFixed = placedb.numFixed(); 
		numPlaceBlockages = placedb.numPlaceBlockages();
		if (!analyze_placedb_structure(placedb, numMovable, numFixed, numPlaceBlockages, 
					iopinNodeStart, num_terminal_NIs, num_pins)) {
			dreamplacePrint(kERROR, "Failed to analyze PlaceDB structure\n");
			return false;
		}

		// Seam C.1: GangSTA uses 0-based REAL cells (no top-module sentinel at index 0). totalcells is
		// exactly the movable+fixed node count; pin2cell = node_id (no +1). A celltype of the design
		// name is not a library cell and would make build() fail.
		size_t totalcells = numMovable + numFixed;
		std::string real_design_name = placedb.designName();
		g_netlist_data.design_name = real_design_name;

		if (!setup_cell_data(placedb, totalcells, real_design_name)) {
			dreamplacePrint(kERROR, "Failed to setup cell data\n");
			return false;
		}

		// Setup pin data using DREAMPlace mappings
		if (!setup_pin_data(placedb, iopinNodeStart, pin2node_data, pin2net_data, pin_direct_data, num_pins)) {
			dreamplacePrint(kERROR, "Failed to setup pin data using DREAMPlace mappings\n");
			return false;
		}

		// Setup net data
		if (!setup_net_data(placedb)) {
			dreamplacePrint(kERROR, "Failed to setup net data\n");
			return false;
		}

		// Create the borrowed const char* pointer arrays GangSTA's C API consumes
		if (!create_interface_arrays()) {
			dreamplacePrint(kERROR, "Failed to create interface arrays\n");
			return false;
		}

		// GangSTA classifies a pin as a top port by the ABSENCE of ':' in its name (seam C.2).
		size_t num_ports = 0;
		for (const auto& pn : g_netlist_data.pin_names) {
			if (pn.find(':') == std::string::npos) ++num_ports;
		}

		dreamplacePrint(kINFO, "GangSTA netlist arrays assembled: %zu cells, %zu pins (%zu top ports), %zu nets\n",
				totalcells, g_netlist_data.pin_names.size(), num_ports, g_netlist_data.net_names.size());

		return true;

	} catch (const std::exception& e) {
		dreamplacePrint(kERROR, "Failed to extract DREAMPlace mappings: %s\n", e.what());
		return false;
	}
}
/// @brief Build the timing database for GangSTA
/// @param sta The STA holdings object
/// @param netlistdb The NetlistDB object
/// @return True on success, false on failure
bool TimingGangstaIO::buildTimerDB(GangstaTimer& sta) {
	const size_t num_cells = g_netlist_data.cell_names.size();
	const size_t num_pins  = g_netlist_data.pin_names.size();
	const size_t num_nets  = g_netlist_data.net_names.size();
	if (num_pins == 0 || num_cells == 0) {
		dreamplacePrint(kERROR, "Empty netlist (cells=%zu, pins=%zu)\n", num_cells, num_pins);
		return false;
	}

	// Step 1: Elmore delay calculator (matches the HeteroSTA op's elmore default).
	gangsta_set_delay_calculator(&sta, GANGSTA_DELAY_ELMORE);

	// Step 2: Ingest the in-memory netlist (replaces NetlistDB + heterosta_set_netlistdb). The pointer
	// arrays were materialized by create_interface_arrays() and stay alive in g_netlist_data.
	if (!gangsta_set_netlist_inmem(&sta, g_netlist_data.design_name.c_str(),
			num_cells, g_netlist_data.cell_name_ptrs.data(), g_netlist_data.cell_type_ptrs.data(),
			num_pins, g_netlist_data.pin_name_ptrs.data(), g_netlist_data.pin_directions.data(),
			g_netlist_data.pin2cell_map.data(), g_netlist_data.pin2net_map.data(),
			num_nets, g_netlist_data.net_name_ptrs.data())) {
		dreamplacePrint(kERROR, "gangsta_set_netlist_inmem failed: %s\n", gangsta_last_error(&sta));
		return false;
	}

	// Step 3: Flatten + build the timing graph.
	gangsta_flatten(&sta);
	gangsta_build_graph(&sta);
	if (!gangsta_is_built(&sta)) {
		dreamplacePrint(kERROR, "gangsta_build_graph failed: %s\n", gangsta_last_error(&sta));
		return false;
	}

	// Step 4: Initialize slew values.
	gangsta_zero_slew(&sta);

	dreamplacePrint(kINFO, "Timer database built successfully: %zu cells, %zu pins, %zu nets; gangsta pins=%zu\n",
			num_cells, num_pins, num_nets, gangsta_num_pins(&sta));
	return true;
}
/// @brief analyze PlaceDB structure to identify IO pin starting index
/// @param placedb 
/// @param numMovable 
/// @param numFixed 
/// @param numPlaceBlockages 
/// @param iopinNodeStart 
/// @param num_terminal_NIs 
/// @param num_pins 
/// @return True on success, false on failure
bool TimingGangstaIO::analyze_placedb_structure(PlaceDB& placedb, size_t& numMovable, 
		size_t& numFixed, size_t& numPlaceBlockages, 
		size_t& iopinNodeStart, size_t num_terminal_NIs, 
		size_t num_pins) {

	// Validate num_terminal_NIs
	if (num_terminal_NIs > num_pins) {
		dreamplacePrint(kERROR, "Invalid num_terminal_NIs: %zu > num_pins: %zu\n", num_terminal_NIs, num_pins);
		return false;
	}

	// Calculate starting index of IO pins using num_terminal_NIs
	// Last num_terminal_NIs pins are IO pins
	iopinNodeStart = numMovable + numFixed + numPlaceBlockages;

	dreamplacePrint(kINFO, "PlaceDB structure analysis:\n");
	dreamplacePrint(kINFO, " numMovable: %zu\n", numMovable);
	dreamplacePrint(kINFO, " numFixed: %zu\n", numFixed);
	dreamplacePrint(kINFO, " numPlaceBlockages: %zu\n", numPlaceBlockages);
	dreamplacePrint(kINFO, " num_pins: %zu\n", num_pins);
	dreamplacePrint(kINFO, " num_terminal_NIs: %zu\n", num_terminal_NIs);
	dreamplacePrint(kINFO, " iopinNodeStart: %zu (calculated from num_terminal_NIs)\n", iopinNodeStart);

	return true;
}
/// @brief Setup cell data with correct GangSTA indexing
/// @param placedb 
/// @param totalcells 
/// @param designName 
/// @return True on success, false on failure

bool TimingGangstaIO::setup_cell_data(PlaceDB& placedb, size_t totalcells, const std::string& designName) {
	dreamplacePrint(kINFO, "Setting up cell data with GangSTA 0-based indexing (no top-module sentinel)...\n");
	(void) designName;  // GangSTA has no sentinel cell; the design name is passed to set_netlist_inmem.

	g_netlist_data.cell_names.reserve(totalcells);
	g_netlist_data.cell_types.reserve(totalcells);

	// Seam C.1: cells are the REAL movable+fixed nodes, 0-based (no empty cell 0 = design name). A
	// celltype of the design name is not a library cell and would make GangSTA's build() fail.
	for (size_t i = 0; i < totalcells; ++i) {
		const auto& node_property = placedb.nodeProperty(i);

		// Cell name: use node name or generate one
		std::string cell_name = node_property.name().empty() ?
			("cell_" + std::to_string(i)) : node_property.name();
		g_netlist_data.cell_names.push_back(cell_name);

		// Cell type: get macro (library cell) name
		std::string cell_type = "UNKNOWN";
		if (node_property.macroId() < placedb.macros().size()) {
			cell_type = placedb.macros()[node_property.macroId()].name();
		}
		g_netlist_data.cell_types.push_back(cell_type);
	}

	dreamplacePrint(kINFO, "Cell data setup complete: %zu cells (0-based real cells)\n", totalcells);
	return true;
}

/// @brief Setup pin data using DREAMPlace mappings for consistency
/// @param placedb 
/// @param iopinNodeStart 
/// @param pin2node_data    
/// @param pin2net_data
/// @param pin_direct_data
/// @param num_pins
/// @return True on success, false on failure

bool TimingGangstaIO::setup_pin_data(PlaceDB& placedb, size_t iopinNodeStart, 
		const int32_t* pin2node_data, const int32_t* pin2net_data, 
		const uint8_t* pin_direct_data, size_t num_pins) {
	dreamplacePrint(kINFO, "Setting up pin data using DREAMPlace mappings\n");

	const auto& pins = placedb.pins();
	const auto& nodes = placedb.nodes();
	// Verify consistency between DREAMPlace and PlaceDB
	if (num_pins != pins.size()) {
		dreamplacePrint(kERROR, "Pin count mismatch: DREAMPlace=%zu, PlaceDB=%zu\n", num_pins, pins.size());
		return false;
	}

	g_netlist_data.pin_names.reserve(num_pins);
	g_netlist_data.pin_directions.reserve(num_pins);
	g_netlist_data.pin2cell_map.reserve(num_pins);
	g_netlist_data.pin2net_map.reserve(num_pins);

	size_t valid_port_count = 0;
	size_t instance_pin_count = 0;

	// Process each pin using DREAMPlace mappings
	for (size_t pin_idx = 0; pin_idx < num_pins; ++pin_idx) {
		const auto& pin = pins[pin_idx];

		// Get DREAMPlace node ID, net ID, and pin direction for this pin
		int32_t dreamplace_node_id = pin2node_data[pin_idx];
		int32_t dreamplace_net_id = pin2net_data[pin_idx];
		uint8_t dreamplace_pin_direct = pin_direct_data[pin_idx];

		// Pin name 
		std::string pin_name;
		bool is_io_pin = (dreamplace_node_id >= 0 && 
				static_cast<size_t>(dreamplace_node_id) >= iopinNodeStart &&
				static_cast<size_t>(dreamplace_node_id) < nodes.size());

		uint8_t direction;
		size_t gangsta_cell_id;

		if (is_io_pin && pin.name().length() > 0) {
			// Top-level port: GangSTA detects it by the ABSENCE of ':' in the name (seam C.2). A port
			// name must therefore be bare (no ':'); pin2cell is IGNORED for ports.
			pin_name = pin.name();
			valid_port_count++;
			// Seam C.3: GangSTA wants a top INPUT port as an OUTPUT pin (1, it DRIVES the netlist) and a
			// top OUTPUT port as an input pin (0, a SINK). DREAMPlace/place_io ALREADY stores top ports
			// with this same inverted convention (an output port's pin_direct is 'INPUT'=0, an input
			// port's is 'OUTPUT'=1), so pass it through directly — NO extra inversion. (Validated on
			// superblue4: inverting here turned output ports into false drivers -> "net has multiple
			// drivers" at build_graph.)
			direction = dreamplace_pin_direct;
			gangsta_cell_id = 0;  // ignored for ports
		} else {
			// Instance pin: KEEP the ':' separator (seam C.2). GangSTA reads the library pin name AFTER
			// ':' and uses pin2cell for the owning instance; direction comes from Liberty (ignored here).
			pin_name = pin.name();
			instance_pin_count++;
			direction = dreamplace_pin_direct;  // ignored for cell pins, but carry it through
			// Seam C.1: pin2cell = node_id (0-based, NO +1 sentinel offset).
			if (dreamplace_node_id >= 0 && dreamplace_node_id < static_cast<int32_t>(nodes.size())) {
				gangsta_cell_id = static_cast<size_t>(dreamplace_node_id);
			} else {
				dreamplacePrint(kWARN, "Invalid node ID %d for pin %zu, using cell ID 0\n", dreamplace_node_id, pin_idx);
				gangsta_cell_id = 0;
			}
		}

		g_netlist_data.pin_names.push_back(pin_name);
		g_netlist_data.pin_directions.push_back(direction);
		g_netlist_data.pin2cell_map.push_back(gangsta_cell_id);
		g_netlist_data.pin2net_map.push_back(static_cast<size_t>(dreamplace_net_id));
	}
	dreamplacePrint(kINFO, "Pin data setup complete: %zu ports, %zu instance pins\n",
			valid_port_count, instance_pin_count);
	return true;
}

/// @brief Setup net data
/// @param placedb 
/// @return True on success, false on failure
bool TimingGangstaIO::setup_net_data(PlaceDB& placedb) {
	dreamplacePrint(kINFO, "Setting up net data...\n");

	const auto& nets = placedb.nets();
	g_netlist_data.net_names.reserve(nets.size());
	g_netlist_data.nets_zero.reserve(0); //zero for temporary use
	g_netlist_data.nets_one.reserve(0); //zero for temporary use

	for (size_t net_idx = 0; net_idx < nets.size(); ++net_idx) {
		const auto& net = nets[net_idx];
		const auto& net_property = placedb.netProperty(net_idx);

		// Net name
		std::string net_name = net_property.name();
		g_netlist_data.net_names.push_back(net_name);

		// Nets zero/one (power/ground nets, typically 0 for normal nets)
		//g_netlist_data.nets_zero.push_back(0);
		//g_netlist_data.nets_one.push_back(0);
	}

	dreamplacePrint(kINFO, "Net data setup complete: %zu nets\n", nets.size());
	return true;
}

/// Create interface arrays for NetlistDB
bool TimingGangstaIO::create_interface_arrays() {
	// Create const char* arrays for NetlistDB interface
	g_netlist_data.cell_name_ptrs.reserve(g_netlist_data.cell_names.size());
	g_netlist_data.cell_type_ptrs.reserve(g_netlist_data.cell_types.size());
	g_netlist_data.pin_name_ptrs.reserve(g_netlist_data.pin_names.size());
	g_netlist_data.net_name_ptrs.reserve(g_netlist_data.net_names.size());

	for (const auto& name : g_netlist_data.cell_names) {
		g_netlist_data.cell_name_ptrs.push_back(name.c_str());
	}
	for (const auto& type : g_netlist_data.cell_types) {
		g_netlist_data.cell_type_ptrs.push_back(type.c_str());
	}
	for (const auto& name : g_netlist_data.pin_names) {
		g_netlist_data.pin_name_ptrs.push_back(name.c_str());
	}
	for (const auto& name : g_netlist_data.net_names) {
		g_netlist_data.net_name_ptrs.push_back(name.c_str());
	}

	return true;
}

/// @brief initialize GangSTA instance with logging callback. GangSTA is open-source — no license.
TimingGangstaIO::GangstaTimerPtr TimingGangstaIO::initialize_heterosta() {
	gangsta_init_logger(dreamplace_gangsta_print_callback);

	auto sta = GangstaTimerPtr(gangsta_new(), gangsta_free);
	if (!sta) {
		dreamplacePrint(kERROR, "Failed to create GangSTA instance\n");
		return GangstaTimerPtr(nullptr, gangsta_free);
	}

	dreamplacePrint(kINFO, "GangSTA initialization completed\n");
	return sta;
}



// Implementation of pin name accessor functions
const char* TimingGangstaIO::getPinName(size_t pin_index) {
	if (pin_index < g_netlist_data.pin_names.size()) {
		return g_netlist_data.pin_names[pin_index].c_str();
	}
	return "unknown";
}

size_t TimingGangstaIO::getPinCount() {
	return g_netlist_data.pin_names.size();
}

/// @brief Build the GangSTA<->DREAMPlace pin permutation by name (seam C.4).
bool TimingGangstaIO::build_pin_maps(GangstaTimer& sta) {
	const size_t num_d = g_netlist_data.pin_names.size();
	const size_t num_g = gangsta_num_pins(&sta);

	// name -> dreamplace pin id (DREAMPlace pin order is g_netlist_data.pin_names insertion order).
	std::unordered_map<std::string, int32_t> name2d;
	name2d.reserve(num_d * 2);
	for (size_t d = 0; d < num_d; ++d) name2d.emplace(g_netlist_data.pin_names[d], static_cast<int32_t>(d));

	g2d_.assign(num_g, -1);
	d2g_.assign(num_d, -1);

	size_t mapped = 0, unmatched_g = 0;
	for (size_t g = 0; g < num_g; ++g) {
		const char* gn = gangsta_pin_name(&sta, g);
		if (!gn || gn[0] == '\0') { ++unmatched_g; continue; }
		auto it = name2d.find(gn);
		if (it == name2d.end()) { ++unmatched_g; continue; }
		const int32_t d = it->second;
		g2d_[g] = d;
		if (d2g_[d] < 0) { d2g_[d] = static_cast<int32_t>(g); ++mapped; }
	}

	size_t unmatched_d = 0;
	for (size_t d = 0; d < num_d; ++d) if (d2g_[d] < 0) ++unmatched_d;

	dreamplacePrint(kINFO, "Pin permutation built: dreamplace=%zu gangsta=%zu mapped=%zu "
			"(unmatched dreamplace=%zu, unmatched gangsta=%zu)\n",
			num_d, num_g, mapped, unmatched_d, unmatched_g);
	return unmatched_d == 0;
}

DREAMPLACE_END_NAMESPACE
