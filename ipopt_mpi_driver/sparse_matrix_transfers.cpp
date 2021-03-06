#include "data_distribution.h"
#include "globals.h"
#include "rank_local_data.h"
#include "sparse_matrix_transfers.h"
#include "trots.h"

#include <iostream>
#include <vector>
#include <unordered_set>

#ifdef USE_MKL
#include "MKL_sparse_matrix.h"
#else
#include "EigenSparseMat.h"
#endif

namespace {
    int probe_message_size(enum MPIMessageTags tag, MPI_Comm communicator, MPI_Datatype type, int rank) {
        MPI_Status status;
        MPI_Probe(rank, tag, communicator, &status);
        int size;
        MPI_Get_count(&status, type, &size);
        return size;
    }

    void distribute_matrices(const TROTSProblem& trots_problem, MPI_Comm communicator,
                             const std::vector<std::unordered_set<int>>& rank_data_id_distribution) {
        //NOTE: We exclude rank 0 here to avoid deadlock problems.
        for (int rank = 1; rank < rank_data_id_distribution.size(); ++rank) {
            const std::unordered_set<int>& data_ids = rank_data_id_distribution[rank];
            int num_matrices = static_cast<int>(data_ids.size());
            MPI_Send(&num_matrices, 1, MPI_INT, rank, NUM_MATS_TAG, communicator);
            for (int data_id : data_ids) {
                const std::variant<std::unique_ptr<SparseMatrix<double>>, std::vector<double>>& data =
                    trots_problem.get_mat_by_data_id(data_id);
                const int is_vec = static_cast<int>(std::holds_alternative<std::vector<double>>(data));
                //First communicate what we're sending, since it can be either a sparse matrix or an array
                MPI_Send(&is_vec, 1, MPI_INT, rank, VEC_FLAG_TAG, communicator);
                MPI_Send(&data_id, 1, MPI_INT, rank, DATA_ID_TAG, communicator);

                if (is_vec) {
                    const auto& vec = std::get<std::vector<double>>(data);
                    MPI_Send(&vec[0], vec.size(), MPI_DOUBLE, rank, VEC_DATA_TAG, communicator);
                }

                else {
                    const auto& mat = std::get<std::unique_ptr<SparseMatrix<double>>>(data);
                    const int nnz = mat->get_nnz();
                    const int num_rows = mat->get_rows();
                    const int num_cols = mat->get_cols();

                    const double* mat_data = mat->get_data_ptr();
                    const int* mat_col_inds = mat->get_col_inds();
                    const int* mat_row_ptrs = mat->get_row_ptrs();

                    MPI_Request requests[4];
                    MPI_Isend(&num_cols, 1, MPI_INT, rank, CSR_NUM_COLS_TAG, communicator, &requests[0]);
                    MPI_Isend(mat_data, nnz, MPI_DOUBLE, rank, CSR_DATA_TAG, communicator, &requests[1]);
                    MPI_Isend(mat_col_inds, nnz, MPI_INT, rank, CSR_COL_INDS_TAG, communicator, &requests[2]);
                    MPI_Isend(mat_row_ptrs, num_rows + 1, MPI_INT, rank, CSR_ROW_PTRS_TAG, communicator, &requests[3]);
                    MPI_Waitall(4, requests, MPI_STATUSES_IGNORE);
                }
            }
        }
    }

    void recv_matrices_for_comm(LocalData& local_data, MPI_Comm communicator) {
        int num_matrices = 0;
        MPI_Recv(&num_matrices, 1, MPI_INT, 0, NUM_MATS_TAG, communicator, MPI_STATUS_IGNORE);
        for (int i = 0; i < num_matrices; ++i) {
            int is_vec;
            MPI_Recv(&is_vec, 1, MPI_INT, 0, VEC_FLAG_TAG, communicator, MPI_STATUS_IGNORE);
            int data_id;
            MPI_Recv(&data_id, 1, MPI_INT, 0, DATA_ID_TAG, communicator, MPI_STATUS_IGNORE);
            if (is_vec) {
                int num_elems = probe_message_size(VEC_DATA_TAG, communicator, MPI_DOUBLE, 0);
                std::vector<double> new_vec(num_elems);
                MPI_Recv(static_cast<void*>(&new_vec[0]), num_elems, MPI_DOUBLE,
                                            0, VEC_DATA_TAG, communicator, MPI_STATUS_IGNORE);
                local_data.mean_vecs.insert({data_id, new_vec});
            }

            else {
                int num_cols;
                MPI_Recv(&num_cols, 1, MPI_INT, 0, CSR_NUM_COLS_TAG, communicator, MPI_STATUS_IGNORE);
                int nnz = probe_message_size(CSR_DATA_TAG, communicator, MPI_DOUBLE, 0);
                int num_rows = probe_message_size(CSR_ROW_PTRS_TAG, communicator, MPI_INT, 0) - 1;

                double* data_buffer = new double[nnz];
                int* col_idxs_buffer = new int[nnz];
                int* row_ptrs_buffer = new int[num_rows + 1];

                MPI_Recv(data_buffer, nnz, MPI_DOUBLE, 0, CSR_DATA_TAG, communicator, MPI_STATUS_IGNORE);
                MPI_Recv(col_idxs_buffer, nnz, MPI_INT, 0, CSR_COL_INDS_TAG, communicator, MPI_STATUS_IGNORE);
                MPI_Recv(row_ptrs_buffer, num_rows + 1, MPI_INT, 0, CSR_ROW_PTRS_TAG, communicator, MPI_STATUS_IGNORE);
#ifdef USE_MKL
                std::unique_ptr<SparseMatrix<double>> mat =
                    MKL_sparse_matrix<double>::from_CSR_mat(nnz, num_rows, num_cols,
                        data_buffer, col_idxs_buffer, row_ptrs_buffer);
#else

                std::unique_ptr<SparseMatrix<double>> mat =
                    EigenSparseMat<double>::from_CSR_mat(nnz, num_rows, num_cols,
                        data_buffer, col_idxs_buffer, row_ptrs_buffer);
#endif
                local_data.matrices.insert(
                    {data_id, std::move(mat)}
                );

                delete[] data_buffer;
                delete[] col_idxs_buffer;
                delete[] row_ptrs_buffer;
            }
        }
    }

    void print_distribution_info(const std::vector<std::vector<int>>& buckets,
                                 const std::vector<TROTSEntry>& entries) {
        int idx = 0;
        for (const std::vector<int>& vec : buckets) {
            int nnz_sum = 0;
            std::cout << "Bucket number " << idx << " elements:\n";
            for (int i : vec) {
                std::cout << i << ",";
                nnz_sum += entries[i].get_nnz();
            }
            std::cout << " nnz sum: " << nnz_sum << "\n\n";
            ++idx;
        }
    }
}

void distribute_sparse_matrices_send(
        TROTSProblem& trots_problem,
        const std::vector<std::vector<int>>& rank_distrib_obj,
        const std::vector<std::vector<int>>& rank_distrib_cons) {
    //Check that we're rank 0
    int world_rank = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    int num_ranks = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
    assert(world_rank == 0);

    //Step 1: post the sends for the data matrices to the correct ranks.
    //Figure out which matrix goes where
    std::vector<std::unordered_set<int>> data_id_buckets(num_ranks);
    for (int i = 0; i < num_ranks; ++i) {
        const std::vector<int>& entry_idxs = rank_distrib_obj[i];
        for (const int entry_idx : entry_idxs) {
            const int data_id = trots_problem.objective_entries[entry_idx].get_id();
            data_id_buckets[i].insert(data_id);
        }
    }
    for (int i = 0; i < num_ranks; ++i) {
        const std::vector<int>& entry_idxs = rank_distrib_cons[i];
        for (const int entry_idx : entry_idxs) {
            const int data_id = trots_problem.constraint_entries[entry_idx].get_id();
            data_id_buckets[i].insert(data_id);
        }
    }

    //Post the sends for the data matrices
    distribute_matrices(trots_problem, MPI_COMM_WORLD, data_id_buckets);
}

void receive_sparse_matrices(LocalData& local_data) {
    recv_matrices_for_comm(local_data, MPI_COMM_WORLD);
}