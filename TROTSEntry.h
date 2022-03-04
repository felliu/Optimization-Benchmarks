#ifndef TROTS_ENTRY_H
#define TROTS_ENTRY_H

#include <variant>

#include "SparseMat.h"

enum class FunctionType {
    Min, Max, Mean, Quadratic,
    gEUD, LTCP, DVH, Chain
};

class TROTSEntry {
public:
    TROTSEntry(matvar_t* problem_struct_entry, matvar_t* data_struct,
               const std::vector<std::variant<std::unique_ptr<SparseMatrix<double>>,
                                              std::vector<double>>
                                >& mat_refs);
    bool is_constraint() const noexcept { return this->is_cons; }
    bool is_active() const noexcept { return this->active; }
    double calc_value(const double* x) const;
    double get_weight() const { return this->weight; }
    void calc_gradient(const double* x, double* grad) const;
    std::vector<double> calc_sparse_grad(const double* x) const;
    //Returns the indexes of the non-zero elements in the gradient of the entry.
    FunctionType function_type() const noexcept { return this->type; }
    std::string get_roi_name() const { return this->roi_name; }

    std::vector<int> get_grad_nonzero_idxs() const { return this->grad_nonzero_idxs; }
private:
    double calc_quadratic(const double* x) const;
    double calc_max(const double* x) const;
    double calc_min(const double* x) const;
    double calc_mean(const double* x) const;
    double calc_LTCP(const double* x) const;
    double calc_gEUD(const double* x) const;

    double quadratic_penalty_min(const double* x) const;
    double quadratic_penalty_max(const double* x) const;
    double quadratic_penalty_mean(const double* x) const;
    void mean_grad(const double* x, double* grad) const;
    void LTCP_grad(const double* x, double* grad, bool cached_dose) const;
    void gEUD_grad(const double* x, double* grad, bool cached_dose) const;
    void quad_min_grad(const double* x, double* grad, bool cached_dose) const;
    void quad_max_grad(const double* x, double* grad, bool cached_dose) const;
    void quad_grad(const double* x, double* grad) const;

    std::vector<int> calc_grad_nonzero_idxs() const;

    int num_vars;
    int id;
    std::string roi_name;
    std::vector<double> func_params;

    std::vector<int> grad_nonzero_idxs;

    bool active;
    bool minimise;
    bool is_cons;

    FunctionType type;
    double rhs;
    double weight;
    //Multiple objectives / constraints can use the same dose deposition matrix. To avoid storing duplicates,
    //all the matrices are stored in TROTSProblem, and the TROTSEntries each have a reference to their matrix instead.
    //const std::variant<MKL_sparse_matrix<double>, std::vector<double>>* matrix_ref;
    double c; //Scalar factor used in quadratic cost functions.
    //const MKL_sparse_matrix<double>* matrix_ref;
    const SparseMatrix<double>* matrix_ref;
    const std::vector<double>* mean_vec_ref;

    //When calculating many objective values, a temporary store for the A*x is needed. Provide it here once so it does not
    //need to be allocated every time.
    mutable std::vector<double> y_vec;
    //Gradient calculation can require more temporaries
    mutable std::vector<double> grad_tmp;
};


#endif
