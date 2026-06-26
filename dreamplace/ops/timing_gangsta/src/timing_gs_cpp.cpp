#include "timing_gs_cpp.h"
#include "timing_gs_io_cpp.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <fstream>
#include <limits>
#include <vector>

DREAMPLACE_BEGIN_NAMESPACE

#ifdef CUDA_FOUND
// Forward declaration of CUDA launcher
template <typename T>
void updateNetWeightCudaLauncher(
    GangstaTimer* sta,
    int num_nets,
    int num_pins,
    const int* flat_netpin,
    const int* netpin_start,
    const int* pin_to_net_map,
    T* net_criticality,
    T* net_criticality_deltas,
    T* net_weights,
    T* net_weight_deltas,
    const int* degree_map,
    T momentum_decay_factor,
    T max_net_weight,
    int ignore_net_degree);
#endif


/// 
/// @brief Perform timing analysis using GangSTA engine.
///// @param sta the GangSTA holdings object containing timing database.
///// @param x the horizontal coordinates of cell locations.
///// @param y the vertical coordinates of cell locations.
///// @param num_pins the number of pins in the design.
///// @param wire_resistance_per_micron unit-length resistance value.
///// @param wire_capacitance_per_micron unit-length capacitance value.
///// @param scale_factor the scaling factor to be applied to the design.
///// @param lef_unit the unit distance microns defined in the LEF file.
///// @param def_unit the unit distance microns defined in the DEF file.
///// @param slacks_rf output array for pin slacks (rise/fall).
///// @param ignore_net_degree the degree threshold for ignoring high-degree nets.
///// @param use_cuda whether to use CUDA for computation.
///
template <typename T>
int timingHeterostaCppLauncher(
		GangstaTimer& sta,
		const T* x,const T* y,
		size_t num_pins,
		T wire_resistance_per_micron,
		T wire_capacitance_per_micron,
		double scale_factor, int lef_unit, int def_unit,
		float (*slacks_rf)[2],
		int ignore_net_degree, bool use_cuda){
	dreamplacePrint(kINFO, "GangSTA launcher started\n");

	// Seam C.5: convert per-micron R/C into GangSTA's ff / kohm per DEF-unit distance. The 1e3 / 1e-15
	// factors take ohm->kohm and F->ff; dividing by unit_to_micron rescales "per micron" into "per DEF
	// coordinate unit" so a Manhattan distance in DEF units yields ff/kohm. (GangSTA's extract_rc star
	// model expects exactly ff/kohm, matching HeteroSTA's canonical units here — no extra scaling.)
	double unit_to_micron = scale_factor * def_unit;
	double res_unit = 1e3;
	double cap_unit = 1e-15;
	double rf = static_cast<double>(wire_resistance_per_micron) / res_unit;
	double cf = static_cast<double>(wire_capacitance_per_micron) / cap_unit;
	double unit_cap_xy = cf / unit_to_micron;
	double unit_res_xy = rf / unit_to_micron;

	auto beg = std::chrono::steady_clock::now();
	(void) use_cuda;  // GangSTA path is CPU-only; coords arrive as host floats.

	// Seam C.4: gather per-pin coords into GangSTA pin order (xs_g[g] = x[ g2d[g] ]).
	const size_t num_g = gangsta_num_pins(&sta);
	std::vector<float> xs_g(num_g, 0.0f), ys_g(num_g, 0.0f);
	for (size_t g = 0; g < num_g; ++g) {
		int32_t d = (g < TimingGangstaIO::g2d_.size()) ? TimingGangstaIO::g2d_[g] : -1;
		if (d >= 0 && static_cast<size_t>(d) < num_pins) {
			xs_g[g] = static_cast<float>(x[d]);
			ys_g[g] = static_cast<float>(y[d]);
		}
	}

	dreamplacePrint(kINFO, "extract rc from placement (gangsta pins=%zu)...\n", num_g);
	const float via_res = 0.0f;
	const uint32_t flute_accuracy = 8;
	const float pdr_alpha = 0.3f;
	const uint8_t use_flute_or_pdr = 0;
	gangsta_extract_rc_from_placement(&sta, xs_g.data(), ys_g.data(),
			static_cast<float>(unit_cap_xy), static_cast<float>(unit_cap_xy),
			static_cast<float>(unit_res_xy), static_cast<float>(unit_res_xy),
			via_res, flute_accuracy, pdr_alpha, use_flute_or_pdr, /*use_gpu=*/false);
	if (const char* e = gangsta_last_error(&sta); e && e[0]) {
		dreamplacePrint(kWARN, "extract_rc_from_placement: %s\n", e);
	}

	gangsta_update_delay(&sta, false);
	gangsta_update_arrivals(&sta, false);
	dreamplacePrint(kINFO, "finish state updates...\n");

	// Report pin slacks in GangSTA order, then scatter back to DREAMPlace pin order (seam C.4).
	if (slacks_rf != nullptr) {
		std::vector<float> sg(num_g * 2, 0.0f);
		gangsta_report_slacks(&sta, GANGSTA_MAX, reinterpret_cast<float(*)[2]>(sg.data()), false);
		for (size_t d = 0; d < num_pins; ++d) {
			int32_t g = (d < TimingGangstaIO::d2g_.size()) ? TimingGangstaIO::d2g_[d] : -1;
			if (g >= 0 && static_cast<size_t>(g) < num_g) {
				slacks_rf[d][0] = sg[g * 2 + 0];
				slacks_rf[d][1] = sg[g * 2 + 1];
			} else {
				slacks_rf[d][0] = slacks_rf[d][1] = 0.0f;
			}
		}
		dreamplacePrint(kDEBUG, "GangSTA slack report completed (scattered to DREAMPlace order)\n");
	}
	auto end = std::chrono::steady_clock::now();
	auto usc = std::chrono::duration_cast<std::chrono::milliseconds>(end - beg);
	dreamplacePrint(kINFO, "finish state updates (%f s)\n", usc.count() * 0.001);
	return 0;
}

// Implementation of the forward method
void TimingGangstaCpp::forward(
		GangstaTimer& sta, torch::Tensor pos,
		size_t num_pins,
		double wire_resistance_per_micron,
		double wire_capacitance_per_micron,
		double scale_factor, int lef_unit, int def_unit,
		torch::Tensor slacks_rf,
		int ignore_net_degree, bool use_cuda) {
	// Check tensor properties for input validation
	CHECK_EVEN(pos);
	CHECK_CONTIGUOUS(pos);
	// Check slack tensor if provided
	float (*slacks_rf_ptr)[2] = nullptr;
	if (slacks_rf.numel() > 0) {
		CHECK_CONTIGUOUS(slacks_rf);
		float* slacks_rf_data = slacks_rf.data_ptr<float>();
		slacks_rf_ptr = reinterpret_cast<float (*)[2]>(slacks_rf_data);
	}

	// GangSTA is CPU-only: hand it a contiguous CPU float32 copy of the per-pin coordinates (laid out
	// [all x | all y]). The launcher permutes DREAMPlace->GangSTA pin order internally (seam C.4) and
	// reports back in DREAMPlace order, so use_cuda only affects whether the COORDS originate on GPU.
	auto pos_cpu = pos.detach().to(torch::kCPU).to(torch::kFloat32).contiguous();
	const float* xc = pos_cpu.data_ptr<float>();
	const float* yc = xc + pos_cpu.numel() / 2;
	timingHeterostaCppLauncher<float>(
			sta, xc, yc, num_pins,
			static_cast<float>(wire_resistance_per_micron),
			static_cast<float>(wire_capacitance_per_micron),
			scale_factor, lef_unit, def_unit,
			slacks_rf_ptr, ignore_net_degree, /*use_cuda=*/false);
}

///
/// @brief Update net weights based on timing criticality using GangSTA (template launcher).
/// @param sta the GangSTA holdings object containing timing database.
/// @param n the maximum number of critical paths to analyze.
/// @param num_nets the number of nets in the design.
/// @param num_pins the number of pins in the design.
/// @param flat_netpin the flattened netpin array.
/// @param netpin_start the starting indices for each net.
/// @param pin_to_net_map the pin to net mapping array.
/// @param net_criticality the criticality values of nets (array).
/// @param net_criticality_deltas the criticality delta values of nets (array).
/// @param net_weights the weights of nets (array).
/// @param net_weight_deltas the increment of net weights.
/// @param degree_map the degree map of nets.
/// @param max_net_weight the maximum net weight in timing optimization.
/// @param momentum_decay_factor the decay factor in momentum iteration.
/// @param ignore_net_degree the net degree threshold for ignoring high-degree nets.
/// @param num_threads number of threads for parallel computing.
///
template <typename T>
void updateNetWeightCppLauncher(
		GangstaTimer& sta,
		int num_nets, 
		int num_pins,
		const int* flat_netpin, 
		const int* netpin_start,
		const int* pin_to_net_map,
		T* net_criticality, 
		T* net_criticality_deltas,
		T* net_weights, 
		T* net_weight_deltas,
		const int* degree_map,
		T momentum_decay_factor, 
		T max_net_weight,
		int ignore_net_degree, 
		int num_threads) {

	// Get WNS/TNS from GangSTA (setup / max corner).
	float wns = 0.f, tns = 0.f;
	bool success = gangsta_report_wns_tns(&sta, GANGSTA_MAX, &wns, &tns, false);
	(void) success;

	// Get pin slacks in GangSTA order, then scatter to DREAMPlace pin order (seam C.4) so the
	// flat_netpin-indexed net loop below reads the right slack per pin.
	const size_t num_g = gangsta_num_pins(&sta);
	static std::vector<float> slack_g;
	if (slack_g.size() < num_g * 2) slack_g.resize(num_g * 2);
	gangsta_report_slacks(&sta, GANGSTA_MAX, reinterpret_cast<float(*)[2]>(slack_g.data()), false);

	static std::vector<float> slack_data;
	if (slack_data.size() < static_cast<size_t>(num_pins) * 2) slack_data.resize(static_cast<size_t>(num_pins) * 2);
	float (*slack_array)[2] = reinterpret_cast<float(*)[2]>(slack_data.data());
	for (int d = 0; d < num_pins; ++d) {
		int32_t g = (static_cast<size_t>(d) < TimingGangstaIO::d2g_.size()) ? TimingGangstaIO::d2g_[d] : -1;
		if (g >= 0 && static_cast<size_t>(g) < num_g) {
			slack_array[d][0] = slack_g[g * 2 + 0];
			slack_array[d][1] = slack_g[g * 2 + 1];
		} else {
			slack_array[d][0] = slack_array[d][1] = std::numeric_limits<float>::max();
		}
	}

	// Apply net weighting using momentum-based criticality update
	if(!(wns < 0)) wns = 0;

#pragma omp parallel for num_threads(num_threads)
	for(int net_i = 0; net_i < num_nets; ++net_i) {

		float net_slack = std::numeric_limits<float>::max();
		int np_s = netpin_start[net_i], np_e = netpin_start[net_i + 1];

		// Calculate net slack as minimum of all pin slacks in the net
		for(int np_i = np_s; np_i < np_e; ++np_i) {
			int pin_i = flat_netpin[np_i];
			if (pin_i < num_pins) {
				// Take the worst (minimum) of rise/fall slack for this pin
				float pin_worst_slack = std::min(slack_array[pin_i][0], 
						slack_array[pin_i][1]);
				net_slack = std::min(net_slack, pin_worst_slack);
			}
		}


		if(wns < 0) {
			// Calculate normalized criticality
			float nc = (net_slack < 0) ? std::max(0.f, net_slack / wns) : 0;
			// Apply momentum-based criticality update 
			net_criticality[net_i] = std::pow(1 + net_criticality[net_i], momentum_decay_factor) * 
				std::pow(1 + nc, 1 - momentum_decay_factor) - 1;
		}

		if(degree_map[net_i]>ignore_net_degree) continue;
		net_weights[net_i] *= (1 + net_criticality[net_i]);

		if(net_weights[net_i] > max_net_weight)        net_weights[net_i] = max_net_weight; 
	}

}

// Implementation of the update_net_weights method
void TimingGangstaCpp::update_net_weights(
		GangstaTimer& sta, int n,
		int num_nets,
		int num_pins,
		torch::Tensor flat_netpin, torch::Tensor netpin_start,
		torch::Tensor pin2net_map,
		torch::Tensor net_criticality, torch::Tensor net_criticality_deltas,
		torch::Tensor net_weights, torch::Tensor net_weight_deltas,
		torch::Tensor degree_map,
		double max_net_weight,
		double momentum_decay_factor,
		int ignore_net_degree, bool use_cuda) {

	// Check tensor properties
	CHECK_CONTIGUOUS(flat_netpin);
	CHECK_CONTIGUOUS(netpin_start);
	CHECK_CONTIGUOUS(pin2net_map);
	CHECK_CONTIGUOUS(net_criticality);
	CHECK_CONTIGUOUS(net_weights);
	CHECK_CONTIGUOUS(net_weight_deltas);
	CHECK_CONTIGUOUS(degree_map);

	// GangSTA is CPU-only and renumbers pins at build (seam C.4), so the net-weight update ALWAYS runs
	// on the CPU launcher (which applies the gangsta->dreamplace slack permutation). use_cuda only
	// governs where the criticality tensors live; we move them to CPU, update, and copy back.
	(void) use_cuda;
	auto fnp = flat_netpin.to(torch::kCPU).to(torch::kInt32).contiguous();
	auto nps = netpin_start.to(torch::kCPU).to(torch::kInt32).contiguous();
	auto p2n = pin2net_map.to(torch::kCPU).to(torch::kInt32).contiguous();
	auto dgm = degree_map.to(torch::kCPU).to(torch::kInt32).contiguous();
	auto nc  = net_criticality.to(torch::kCPU).contiguous();
	auto ncd = net_criticality_deltas.to(torch::kCPU).contiguous();
	auto nw  = net_weights.to(torch::kCPU).contiguous();
	auto nwd = net_weight_deltas.to(torch::kCPU).contiguous();

	DREAMPLACE_DISPATCH_FLOATING_TYPES(
			nc, "updateNetWeightCppLauncher",
			[&] {
			updateNetWeightCppLauncher<scalar_t>(
					sta,
					num_nets,
					num_pins,
					DREAMPLACE_TENSOR_DATA_PTR(fnp, int),
					DREAMPLACE_TENSOR_DATA_PTR(nps, int),
					DREAMPLACE_TENSOR_DATA_PTR(p2n, int),
					DREAMPLACE_TENSOR_DATA_PTR(nc, scalar_t),
					DREAMPLACE_TENSOR_DATA_PTR(ncd, scalar_t),
					DREAMPLACE_TENSOR_DATA_PTR(nw, scalar_t),
					DREAMPLACE_TENSOR_DATA_PTR(nwd, scalar_t),
					DREAMPLACE_TENSOR_DATA_PTR(dgm, int),
					static_cast<scalar_t>(momentum_decay_factor),
					static_cast<scalar_t>(max_net_weight),
					ignore_net_degree,
					at::get_num_threads());
			});

	// Copy the updated criticality/weights back into the caller's (possibly GPU) tensors.
	net_criticality.copy_(nc);
	net_criticality_deltas.copy_(ncd);
	net_weights.copy_(nw);
	net_weight_deltas.copy_(nwd);
}

DREAMPLACE_END_NAMESPACE
