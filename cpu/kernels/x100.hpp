#ifndef H_KERNEL_X100
#define H_KERNEL_X100

#include "../common.hpp"
#include <cinttypes>

enum AggrFlavour {
	k1Step, kMultiplePrims, kMagic, kMagicFused, kNoAggr
};

enum Avx512Flavour {
	kNoAvx512, kCompare, kPopulationCount
};

template<AggrFlavour aggr_flavour, bool nsm, Avx512Flavour avx512 = kNoAvx512>
struct KernelX100 : BaseKernel {
	static constexpr size_t kVectorsize = MAX_VSIZE;

	idx_t* RESTRICT pos;
	idx_t* RESTRICT lim;
	idx_t* RESTRICT grp;

	uint16_t** grppos;
	uint16_t* selbuf;

	int16_t* RESTRICT v_shipdate;
	int8_t* RESTRICT v_returnflag;
	int8_t* RESTRICT v_linestatus;
	int8_t* RESTRICT v_discount;
	int8_t* RESTRICT v_tax;
	int32_t* RESTRICT v_extendedprice;
	int16_t* RESTRICT v_quantity;

	int8_t* RESTRICT v_disc_1;
	int8_t* RESTRICT v_tax_1;

	idx_t* RESTRICT v_idx; // TODO: make int16_t
	int32_t* RESTRICT v_disc_price;
	int64_t*  RESTRICT v_charge;
	sel_t* RESTRICT v_sel;

	kernel_compact_declare

	#define scan(name) v_##name = (l_##name);
	#define scan_epilogue(name) v_##name += chunk_size;

	KernelX100(const lineitem& li, size_t core) : BaseKernel(li) {
		grppos = new_array<uint16_t*>(kGrpPosSize);
		selbuf = new_array<uint16_t>(kSelBufSize);
		pos = new_array<idx_t>(kVectorsize);
		lim = new_array<idx_t>(kVectorsize);
		grp = new_array<idx_t>(kVectorsize);

		kernel_compact_init(core);

		v_shipdate = new_array<int16_t>(kVectorsize);

		v_returnflag = new_array<int8_t>(kVectorsize);
		v_linestatus = new_array<int8_t>(kVectorsize);

		v_shipdate = new_array<int16_t>(kVectorsize);
		v_discount = new_array<int8_t>(kVectorsize);

		v_tax = new_array<int8_t>(kVectorsize);
		v_extendedprice = new_array<int32_t>(kVectorsize);

		v_quantity = new_array<int16_t>(kVectorsize);



		v_disc_1 = new_array<int8_t>(kVectorsize);
		v_tax_1 = new_array<int8_t>(kVectorsize);

		v_idx = new_array<idx_t>(kVectorsize);
		v_disc_price = new_array<int32_t>(kVectorsize);
		v_charge = new_array<int64_t>(kVectorsize);

		v_sel = new_array<sel_t>(kVectorsize);
	}

    struct ExprProf {
#ifdef PROFILE
        size_t tuples = 0;
        size_t time = 0;
#endif
    };
    
    ExprProf prof_select, prof_map_gid, prof_map_disc_1,
    	prof_map_tax_1, prof_map_disc_price, prof_map_charge,
    	prof_aggr_quantity, prof_aggr_base_price, prof_aggr_disc_price,
		prof_aggr_charge, prof_aggr_disc, prof_aggr_count;

#ifdef PROFILE
	int64_t prof_num_full_aggr = 0;
	int64_t prof_num_strides = 0;
#endif

    template<typename T>
    auto ProfileLambda(ExprProf& prof, size_t tuples, T&& fun) {
#ifdef PROFILE
        prof.tuples += tuples;
        auto begin = rdtsc();
        auto result = fun();        
        prof.time += rdtsc() - begin;
        return std::move(result);
#else
        return fun();
#endif
    }
    
	void Profile(size_t total_tuples) override {
#ifdef PROFILE
#define p(name) do { \
		ExprProf& d = prof_##name; \
		if (d.tuples > 0) { printf("%s %f\n", #name, (float)d.time / (float)d.tuples); } \
	} while(false)

		p(select);
		p(map_gid);
		p(map_disc_1);
		p(map_tax_1);
        p(map_disc_price);
        p(map_charge);

        p(aggr_quantity);
		p(aggr_base_price);
		p(aggr_disc_price);
		p(aggr_charge);
		p(aggr_disc);
		p(aggr_count);

		printf("aggr on full vector (no tuples filtered) %" PRId64 "/%" PRId64 "\n", prof_num_full_aggr, prof_num_strides);
#endif
	}

	NOINL void operator()() {
		task(0, li.l_extendedprice.cardinality);
	}

	NOINL void task(size_t offset, size_t morsel_num) {
		kernel_prologue();

		/* Ommitted vector allocation on stack, 
		 * because C++ compilers will produce invalid results together with magic_preaggr (d270d85b8dcef5f295b1c10d4b2336c9be858541)
		 * Moving allocations to class fixed these issues which will be triggered with O1, O2 and O3 */
	
#ifdef GPU
		const int16_t date = (int32_t)cmp.dte_val - (int32_t)727563;
#else
		const int16_t date = cmp.dte_val;
#endif
		const int8_t int8_t_one_discount = (int8_t)Decimal64::ToValue(1, 0);
		const int8_t int8_t_one_tax = (int8_t)Decimal64::ToValue(1, 0);

		scan(shipdate);
		v_shipdate += offset;

		scan(returnflag);
		v_returnflag += offset;

		scan(linestatus);
		v_linestatus += offset;

		scan(discount);
		v_discount += offset;

		scan(tax);
		v_tax += offset;

		scan(extendedprice);
		v_extendedprice += offset;

		scan(quantity);
		v_quantity += offset;


		size_t done=0;
		while (done < morsel_num) {
			sel_t* sel = v_sel;
			const size_t chunk_size = min(kVectorsize, morsel_num - done);

			size_t n = chunk_size;

			const size_t num = ProfileLambda(prof_select, n,
				[&] () { 
					if (avx512 == kNoAvx512) {
						return Primitives::select_int16_t(sel, nullptr, n, false, v_shipdate, date);
					} else {
						return Primitives::select_int16_t_avx512(sel, nullptr, n, false, v_shipdate, date);
					}
				});

			if (!num) {
				scan_epilogue(returnflag);
				scan_epilogue(linestatus);
				scan_epilogue(shipdate);
				scan_epilogue(discount);
				scan_epilogue(tax);
				scan_epilogue(extendedprice);
				scan_epilogue(quantity);
				done += chunk_size;
				continue;
			}

			if (num > kVectorsize / 2) {
				if (num != n)
					n = sel[num-1]+1;
				sel = nullptr;
			} else {
				n = num;
			}

#ifdef PROFILE
			const auto prof_sc_start = rdtsc();
#endif

			ProfileLambda(prof_map_gid, n, [&] () {
				if (avx512 == kNoAvx512) {
					/* Faster version of "Primitives::map_gid(v_idx, sel, n, v_returnflag, v_linestatus);"
					 * But current version cannot print the groupids properly.
					 * Anyway this primitive behaves terrible on KNL as well as the hand-optimized ones (6 cycles/tuple)
					 */
					return Primitives::map_gid2_dom_restrict(v_idx, sel, n, v_returnflag, li.l_returnflag.minmax.min, li.l_returnflag.minmax.max,
						v_linestatus, li.l_linestatus.minmax.min, li.l_linestatus.minmax.max);
				} else {
					return Primitives::map_gid(v_idx, sel, n, v_returnflag, v_linestatus);
				}
			});

			ProfileLambda(prof_map_disc_1, n, [&] () {
				return Primitives::map_disc_1(v_disc_1, sel, n, int8_t_one_discount, v_discount);
			});

			ProfileLambda(prof_map_tax_1, n, [&] () {
				return Primitives::map_tax_1(v_tax_1, sel, n, v_tax, int8_t_one_tax);
			});

			ProfileLambda(prof_map_disc_price, n, [&] () {
				return Primitives::map_disc_price(v_disc_price, sel, n, v_disc_1, v_extendedprice);			
			});

			ProfileLambda(prof_map_charge, n, [&] () {
				return Primitives::map_charge(v_charge, v_disc_price, v_tax_1, sel, n);
			});

#ifdef PROFILE
			const auto prof_ag_start = rdtsc();
#endif

			sel_t* aggr_sel = num == chunk_size ? nullptr /* full vector, nothing filtered */ : v_sel;
#ifdef PROFILE
			prof_num_full_aggr += num == chunk_size;
			prof_num_strides++;
#endif
			switch (aggr_flavour) {
			case kNoAggr:
				break;

			case kMagicFused:
			case kMagic: {
				auto gp0 = grppos;
				auto gp1 = grppos + kGrpPosSize/2;
				auto sb0 = selbuf;
				auto sb1 = selbuf + kSelBufSize/2;

#ifdef PROFILE
				const auto prof_magic_start = rdtsc();
#endif

				size_t num_groups;
				switch (avx512) {
				case kNoAvx512:
					num_groups = Primitives::partial_shuffle_scalar(v_idx, aggr_sel, num, pos, lim, grp, gp0, gp1, sb0, sb1);
					break;
				case kCompare:
					num_groups = Primitives::partial_shuffle_avx512_cmp(v_idx, v_sel, num, pos, lim, grp, gp0, gp1, sb0, sb1);
					break;
				case kPopulationCount:
					num_groups = Primitives::partial_shuffle_avx512(v_idx, v_sel, num, pos, lim, grp, gp0, gp1, sb0, sb1);
					break;
				}

#ifdef PROFILE
				sum_magic_time += rdtsc() - prof_magic_start;
#endif
				/* pre-aggregate */
				if (aggr_flavour == kMagicFused) {
					Primitives::ordaggr_all_in_one(aggrs0, pos, lim, grp, num_groups, v_quantity, v_extendedprice, v_disc_price, v_charge, v_disc_1);
				} else {
					#define aggregate(prof, ag, vec) do { \
							ProfileLambda(prof_aggr_##prof, n, [&] () { \
								return avx512 == kNoAvx512 ? \
									Primitives::ordaggr_##ag(aggrs0, pos, lim, grp, num_groups, vec) : \
									Primitives::par_ordaggr_##ag(aggrs0, pos, lim, grp, num_groups, vec); \
							}); \
						} while (false)

					aggregate(quantity, quantity, v_quantity);
					aggregate(base_price, extended_price, v_extendedprice);
					aggregate(disc_price, disc_price, v_disc_price);
					aggregate(charge, charge, v_charge);
					aggregate(disc, disc, v_disc_1);

					ProfileLambda(prof_aggr_count, n, [&] () {
						return Primitives::ordaggr_count(aggrs0, pos, lim, grp, num_groups);
					});

					#undef aggregate
				}

				break; // kMagic
			}

			case k1Step:
				Primitives::for_each(aggr_sel, num, [&] (auto i) {
					const auto g = v_idx[i];
					if (nsm) {
						aggrs0[g].sum_quantity += v_quantity[i];						
						aggrs0[g].sum_base_price += v_extendedprice[i];
						aggrs0[g].sum_disc_price = int128_add64(aggrs0[g].sum_disc_price, v_disc_price[i]);
						aggrs0[g].sum_charge = int128_add64(aggrs0[g].sum_charge, v_charge[i]);
						aggrs0[g].sum_disc += v_disc_1[i];
						aggrs0[g].count ++;
					} else {
						aggr_dsm0_sum_quantity[g] += v_quantity[i];
						aggr_dsm0_sum_base_price[g] += v_extendedprice[i];
						aggr_dsm0_sum_disc_price[g] = int128_add64(aggr_dsm0_sum_disc_price[g], v_disc_price[i]);
						aggr_dsm0_sum_charge[g] = int128_add64(aggr_dsm0_sum_charge[g], v_charge[i]);
						aggr_dsm0_sum_disc[g] += v_disc_1[i];
						aggr_dsm0_count[g] ++;
					}
				});
				break; // k1Step

			case kMultiplePrims:
				ProfileLambda(prof_aggr_quantity, n, [&] () {
					return Primitives::for_each(aggr_sel, num, [&] (auto i) {
						auto g = v_idx[i];
						if (nsm) {
							aggrs0[g].sum_quantity += v_quantity[i];
						} else {
							aggr_dsm0_sum_quantity[g] += v_quantity[i];
						}
					});		
				});	

				Primitives::for_each(aggr_sel, num, [&] (auto i) {
					auto g = v_idx[i];
					if (nsm) {
						aggrs0[g].sum_base_price += v_extendedprice[i];
					} else {
						aggr_dsm0_sum_base_price[g] += v_extendedprice[i];
					}
				});
				Primitives::for_each(aggr_sel, num, [&] (auto i) {
					auto g = v_idx[i];
					if (nsm) {
						aggrs0[g].sum_disc_price = int128_add64(aggrs0[g].sum_disc_price, v_disc_price[i]);
					} else {
						aggr_dsm0_sum_disc_price[g] = int128_add64(aggr_dsm0_sum_disc_price[g], v_disc_price[i]);
					}
				});
				Primitives::for_each(aggr_sel, num, [&] (auto i) {
					auto g = v_idx[i];
					if (nsm) {
						aggrs0[g].sum_charge = int128_add64(aggrs0[g].sum_charge, v_charge[i]);
					} else {
						aggr_dsm0_sum_charge[g] = int128_add64(aggr_dsm0_sum_charge[g], v_charge[i]);
					}
				});
				Primitives::for_each(aggr_sel, num, [&] (auto i) {
					auto g = v_idx[i];
					if (nsm) {
						aggrs0[g].sum_disc += v_disc_1[i];
					} else {
						aggr_dsm0_sum_disc[g] += v_disc_1[i];
					}
				});
				Primitives::for_each(aggr_sel, num, [&] (auto i) {
					auto g = v_idx[i];
					if (nsm) {
						aggrs0[g].count ++;
					} else {
						aggr_dsm0_count[g]++;
					}
				});
				break;
			}

#ifdef PROFILE
			sum_aggr_time += rdtsc() - prof_ag_start;
#endif
			scan_epilogue(returnflag);
			scan_epilogue(linestatus);
			scan_epilogue(shipdate);
			scan_epilogue(discount);
			scan_epilogue(tax);
			scan_epilogue(extendedprice);
			scan_epilogue(quantity);
			done += chunk_size;
		};
	}

	#undef scan
	#undef scan_epilogue

};

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>

// without stupid typedefs
void precompute_filter_for_table_chunk(
    const uint16_t*  __restrict__  compressed_ship_date,
    uint32_t*                __restrict__  precomputed_filter,
    uint32_t                                 num_tuples);

#include "../../src/cpu.hpp"

template<typename KERNEL, bool full_system = true>
struct Morsel : BaseKernel {

	std::vector<std::thread> workers;
	std::vector<KERNEL*> states;

	size_t num_morsels;
	std::atomic<size_t> morsel_start;
	std::atomic<size_t> morsel_completed;

	std::mutex lock_query;
	std::condition_variable cond_finished;
	std::condition_variable cond_start;

	size_t pushdown_cpu_start_offset;

	enum Stage {
		UNCLEAR = 0,
		DONE,
		QUERY,
		DTOR,
	};

	Stage stage;

	size_t size;

	void FilterPushDownShit(size_t offset, size_t num) {
		static_assert(sizeof(compr_shipdate[0] ) == sizeof(uint16_t), "Wrong type");
		precompute_filter_for_table_chunk(compr_shipdate + offset, precomp_filter + offset / 32, num);

		precomp_filter_queue.enqueue(FilterChunk { offset, num});
	}

	NOINL void Query(size_t id) {
		assert(size > 0);
		auto& s = *states[id];

		// process all morsels
		while (morsel_start < size) {
			size_t offset = morsel_start.fetch_add(morsel_size);
			if (offset >= size) {
				break;
			}

			size_t num = min(morsel_size, size - offset);

			assert(offset < li.l_extendedprice.cardinality);
			assert(offset + num <= li.l_extendedprice.cardinality);

			if (offset < pushdown_cpu_start_offset) {
				FilterPushDownShit(offset, num);
			} else {
				s.task(offset, num);
			}

			if (morsel_completed++ >= num_morsels) {
				std::unique_lock<std::mutex> lock(lock_query);
				cond_finished.notify_all();
				break;
			}
		}

		// propagate partial results to global table
		for (size_t group=0; group<MAX_GROUPS; group++) {
			if (s.aggrs0[group].count > 0) {
				#define FADD(NAME) __sync_fetch_and_add(&aggrs0[group].NAME, s.aggrs0[group].NAME)
				FADD(sum_quantity);
				FADD(sum_base_price);
				FADD(sum_disc);
				FADD(sum_disc_price);
				FADD(sum_charge);
				FADD(count);
			}
		}

		// well ... this worker's job is done
		stage = DONE;
	}

	void Work(size_t id) {
		bool query = false;

		{
			std::unique_lock<std::mutex> lock(lock_query);
			assert(!states[id]);
			states[id] = new KERNEL(li, full_system ? id : id*2);
			cond_finished.notify_all();
		}

		while (true) {
			{
				std::unique_lock<std::mutex> lock(lock_query);

				while (stage != DTOR && stage != QUERY) {
					cond_start.wait(lock);
				}

				query = stage == QUERY;
				if (stage == DTOR) {
					return;
				}
			}

			if (query) {
				Query(id);
			}
		}
		
		delete states[id];
		states[id] = nullptr;
	}
	
	Morsel(const lineitem& li, bool wo_core0 = false) : BaseKernel(li) {
		size_t threadinhos = std::thread::hardware_concurrency();
		threadinhos/=2;

		if (!full_system) {
			threadinhos /= 2;
		}
		if (wo_core0) {
			threadinhos--;
		}

		if (threadinhos < 1) {
			threadinhos = 1;
		}

		{
			std::unique_lock<std::mutex> lock(lock_query);
			stage = DONE;

			states.resize(threadinhos);
			for (size_t i=0; i<threadinhos; i++) {
				states[i] = nullptr;
			}
		}

		{
			std::unique_lock<std::mutex> lock(lock_query);

			for (size_t i=0 ; i<threadinhos; i++) {
				workers.push_back(std::thread(&Morsel::Work, this, i));

#if 0
				// set affinity
				cpu_set_t cpuset;
				CPU_ZERO(&cpuset);
				CPU_SET(i + wo_core0 ? 1 : 0, &cpuset);
				int rc = pthread_setaffinity_np(workers[i].native_handle(), sizeof(cpu_set_t), &cpuset);
				assert(rc == 0 && "Setting affinity failed");
#endif
			}
		}

		{ // wait until all are up and running
			std::unique_lock<std::mutex> lock(lock_query);

			while (true) {
				bool all_up = true;
				for(auto& s : states) {
					all_up &= !!s;
				}	
				if (!all_up) {
					cond_finished.wait(lock);
				} else {
					break;
				}
			}
		}
	}

	void Profile(size_t total_tuples) override {
	}

	NOINL void spawn(size_t offset, size_t num, size_t pushdown_cpu_start_offset) {
		this->pushdown_cpu_start_offset = pushdown_cpu_start_offset;
		size = offset + num;
		assert(size <= li.l_extendedprice.cardinality);
		// start threads
		{
			std::unique_lock<std::mutex> lock(lock_query);
			num_morsels = num / morsel_size;
			if (num_morsels < 1) {
				num_morsels = 1;
			}
			morsel_start = offset;
			morsel_completed = 0;
			stage = QUERY;
			cond_start.notify_all();
		}
	}

	NOINL void wait(bool active = false) {
		if (active) {
			Work(0);
		}
		// wait for completion
		{
			std::unique_lock<std::mutex> lock(lock_query);
			while (morsel_completed < num_morsels) {
				cond_finished.wait(lock);
			}

			stage = DONE;
		}
	}

	NOINL void task(size_t offset, size_t num) {
		spawn(offset, num, 0);

		wait();
	}

	NOINL void operator()() {
		task(0, li.l_extendedprice.cardinality);
	}

	virtual void Clear() override
	{
		for (auto& s : states) {
			if (s) {
				s->Clear();
			}
		}

		BaseKernel::Clear();
	}

	~Morsel() {
		{
			std::unique_lock<std::mutex> lock(lock_query);
			cond_start.notify_all();
			stage = DTOR;
		}

		for (auto& w : workers) {
			if (!w.joinable()) {
				continue;
			}
			w.join();
		}
	}
};


#endif
