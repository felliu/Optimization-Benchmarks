#include <mpi.h>

#include <iostream>
#include <filesystem>
#include <unordered_set>

#include "coin-or/IpIpoptApplication.hpp"


#include "data_distribution.h"
#include "globals.h"
#include "rank_local_data.h"
#include "sparse_matrix_transfers.h"
#include "trots.h"
#include "test_distrib.h"
#include "trots_entry_transfers.h"
#include "trots_ipopt_mpi.h"
#include "util.h"

/*MPI_Comm obj_ranks_comm = MPI_COMM_NULL;
MPI_Comm cons_ranks_comm = MPI_COMM_NULL;*/

namespace {
    void show_rank_local_data(int rank, const LocalData& data) {
        int my_rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
        if (my_rank != rank)
            return;

        std::vector<int> matrix_ids;
        int total_nnz = 0;
        for (const auto& [key, mat] : data.matrices) {
            matrix_ids.push_back(key);
            total_nnz += mat->get_nnz();
        }

        std::vector<int> mean_vec_ids;
        for (const auto& [key, mat] : data.mean_vecs) {
            mean_vec_ids.push_back(key);
            total_nnz += mat.size();
        }

        std::cout << "Local data for rank: " << rank << "\n";
        std::cout << "Matrix ids: ";
        print_vector(matrix_ids);

        std::cout << "Vec ids: ";
        print_vector(mean_vec_ids);

        std::cout << "Total nnz: " << total_nnz << std::endl;
    }

    void show_rank_local_entries(int rank, const LocalData& data) {
        int my_rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
        if (my_rank != rank)
            return;

        /*std::cout << "TROTSentries for rank: " << my_rank << "\n";
        for (const TROTSEntry& entry : data.trots_entries) {
            std::cout << "TrotsEntry name: " << entry.get_roi_name() << "\n";
        }*/
        std::cout << "Objective entries for rank: " << my_rank << "\n";
        for (const TROTSEntry& entry : data.obj_entries)
            std::cout << "TrotsEntry name: " << entry.get_roi_name() << "\n";

        std::cout << "Constraint entries for rank: " << my_rank << "\n";
        for (const TROTSEntry& entry : data.cons_entries)
            std::cout << "TrotsEntry name: " << entry.get_roi_name() << "\n";

        std::cout << "\n" << std::endl;
    }

    void calc_values_test(Ipopt::SmartPtr<Ipopt::TNLP> tnlp, int n, int m) {
        std::vector<double> x(n);
        for (int i = 0; i < n; ++i) {
            x[i] = 100.0;
        }
        double obj_val;
        tnlp->eval_f(n, &x[0], false, obj_val);
        std::vector<double> cons_vals(m);
        tnlp->eval_g(n, &x[0], false, m, &cons_vals[0]);
        std::cout << "Obj_val: " << obj_val << "\n";
        std::cout << "Cons vals: ";
        print_vector(cons_vals);
    }

    int max_iters = 5000;
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);
    int world_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    int num_ranks = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

    std::vector<std::vector<int>> rank_distrib_obj;
    std::vector<std::vector<int>> rank_distrib_cons;
    TROTSProblem trots_problem;
    LocalData rank_local_data;
    if (world_rank == 0) {
        if (argc < 2 || argc > 3) {
            std::cerr << "Usage: ./program <mat_file>\n"
                      << "\t./program <mat_file> <max_iters>\n";
            return -1;
        }

        std::string path_str{argv[1]};
        std::filesystem::path path{path_str};

        if (argc == 3)
            max_iters = std::atoi(argv[2]);

        trots_problem = std::move(TROTSProblem{TROTSMatFileData{path}});

        rank_local_data.num_vars = trots_problem.get_num_vars();

    }

    MPI_Bcast(&rank_local_data.num_vars, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (world_rank == 0) {
        int num_ranks = 0;
        MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

        //Get a roughly even distribution of the matrices between the ranks, excluding rank 0
        rank_distrib_obj = get_rank_distribution(trots_problem.objective_entries, num_ranks);
        assert(rank_distrib_obj[0].empty());
        for (int i = 0; i < rank_distrib_obj.size();++i) {
            const auto& v = rank_distrib_obj[i];
            std::cout << "Rank " << i << " obj entries\n";
            print_vector(v);
        }
        rank_distrib_cons = get_rank_distribution(trots_problem.constraint_entries, num_ranks);
        assert(rank_distrib_cons[0].empty());
        for (int i = 0; i < rank_distrib_cons.size();++i) {
            const auto& v = rank_distrib_cons[i];
            std::cout << "Rank " << i << " cons entries\n";
            print_vector(v);
        }

        //dump_distrib_data_to_file(rank_distrib_obj, rank_distrib_cons, trots_problem);
        distribute_sparse_matrices_send(trots_problem, rank_distrib_obj, rank_distrib_cons);
    }

    if (world_rank != 0)
        receive_sparse_matrices(rank_local_data);

    /*MPI_Barrier(MPI_COMM_WORLD);
    for (int i = 0; i < num_ranks; ++i) {
        show_rank_local_data(i, rank_local_data);
        MPI_Barrier(MPI_COMM_WORLD);
    }*/
    if (world_rank == 0) {
        distribute_trots_entries_send(
            trots_problem.objective_entries,
            trots_problem.constraint_entries,
            rank_distrib_obj,
            rank_distrib_cons);
    }

    if (world_rank != 0) {
        recv_trots_entries(rank_local_data);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    init_local_data(rank_local_data);
    MPI_Barrier(MPI_COMM_WORLD);
    print_local_nnz_count(rank_local_data);

    if (world_rank == 0) {
        const int num_cons = trots_problem.get_num_constraints();
        Ipopt::SmartPtr<Ipopt::TNLP> tnlp =
            new TROTS_ipopt_mpi(std::move(trots_problem), rank_distrib_cons, std::move(rank_local_data));

        Ipopt::SmartPtr<Ipopt::IpoptApplication> app = new Ipopt::IpoptApplication();
        app->Options()->SetStringValue("hessian_approximation", "limited-memory");
        app->Options()->SetStringValue("mu_strategy", "adaptive");
        app->Options()->SetStringValue("adaptive_mu_globalization", "kkt-error");
        app->Options()->SetStringValue("print_timing_statistics", "yes");
        //app->Options()->SetStringValue("derivative_test", "first-order");
        app->Options()->SetIntegerValue("max_iter", max_iters);
        app->Options()->SetNumericValue("tol", 1e-9);
        app->Initialize();
        app->OptimizeTNLP(tnlp);
        //Finally, get the objective and constraint ranks out of their infinite loops
        compute_vals_mpi(true, nullptr, nullptr, false, nullptr, rank_local_data, std::nullopt, true);
    } else {
        compute_vals_mpi(true, nullptr, nullptr, false, nullptr, rank_local_data, std::nullopt, false);
    }

    MPI_Finalize();
    return 0;
}
