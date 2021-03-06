#ifndef TROTS_H
#define TROTS_H

#include <cassert>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#ifdef USE_MKL
#include <mkl.h>
#endif

#include "trots_matfile_data.h"
#include "SparseMat.h"
#include "TROTSEntry.h"


class TROTSProblem {
public:
    TROTSProblem() = default;
    TROTSProblem(TROTSMatFileData&& trots_data);

    //TODO: Make these private
    std::vector<TROTSEntry> objective_entries;
    std::vector<TROTSEntry> constraint_entries;
    int get_num_vars() const noexcept { return this->num_vars; }
    int get_nnz_jac_cons() const noexcept { return this->nnz_jac_cons; }
    int get_num_constraints() const noexcept {
        return this->constraint_entries.size();
    }
    double calc_objective(const double* x, bool cached_dose=false) const;
    void calc_obj_gradient(const double* x, double* y, bool cached_dose=false) const;
    void calc_constraints(const double* x, double* cons_vals, bool cached_dose=false) const;
    void calc_jacobian_vals(const double* x, double* jacobian_vals, bool cached_dose=false) const;
    std::variant<std::unique_ptr<SparseMatrix<double>>, std::vector<double>>&
    get_mat_by_data_id(int data_id) {
        return matrices[data_id - 1];
    }
    const std::variant<std::unique_ptr<SparseMatrix<double>>, std::vector<double>>&
    get_mat_by_data_id(int data_id) const {
        return matrices[data_id - 1];
    }
    void clear_mat_data() {
        this->matrices.clear();
    }


private:
    void read_dose_matrices();

    int num_vars;
    int nnz_jac_cons;
    TROTSMatFileData trots_data;
    //List of matrix entries, indexed by dataID.
    //If the FunctionType is mean, the value is computed using a dot product with a dense vector,
    //In other cases, the dose is calculated using a dose deposition matrix.
    std::vector<std::variant<std::unique_ptr<SparseMatrix<double>>, std::vector<double>>> matrices;
};

#endif
