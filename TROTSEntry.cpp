#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <numeric>
#include <set>
#include <vector>

#include <matio.h>

#include "MKL_sparse_matrix.h"
#include "TROTSEntry.h"
#include "util.h"

namespace {
    const char* func_type_names[] = {"Min", "Max", "Mean", "Quadratic", "gEUD", "LTCP", "DVH", "Chain"};

    FunctionType get_linear_function_type(int dataID, bool minimise, const std::string& roi_name, matvar_t* matrix_struct) {
        const int zero_indexed_dataID = dataID - 1;
        std::cerr << "Reading name field\n";
        //The times when the function type is mean, the data.matrix struct should have an entry at dataID with the name "<ROI_name> + (mean)"...
        matvar_t* matrix_entry_name_var = Mat_VarGetStructFieldByName(matrix_struct, "Name", zero_indexed_dataID);
        check_null(matrix_entry_name_var, "Failed to read name field of matrix entry\n.");

        std::string matrix_entry_name(static_cast<char*>(matrix_entry_name_var->data));
        std::cerr << "Matrix name: " << matrix_entry_name << "\n";

        auto n = matrix_entry_name.find("(mean)");
        if (n != std::string::npos)
            return FunctionType::Mean;

        return minimise ? FunctionType::Max : FunctionType::Min;
    }

    FunctionType get_nonlinear_function_type(int type_id) {
        assert(type_id >= 2);
        //The type ID is one-indexed, so need to subtract one for that.
        //Then add two since type_id one maps to three different possible function types.
        return static_cast<FunctionType>(type_id + 1);
    }
}

TROTSEntry::TROTSEntry(matvar_t* problem_struct_entry, matvar_t* matrix_struct,
                       const std::vector<std::variant<std::unique_ptr<SparseMatrix<double>>,
                                         std::vector<double>>
                                         >& mat_refs) :
    rhs{0}, weight{0}, c{0}
{
    assert(problem_struct_entry->class_type == MAT_C_STRUCT);
    //Try to ensure that the structure is a scalar (1x1) struct.
    assert(problem_struct_entry->rank == 2);
    assert(problem_struct_entry->dims[0] == 1 && problem_struct_entry->dims[1] == 1);

    matvar_t* name_var = Mat_VarGetStructFieldByName(problem_struct_entry, "Name", 0);
    check_null(name_var, "Cannot find name variable in problem entry.");
    assert(name_var->class_type == MAT_C_CHAR);
    this->roi_name = std::string(static_cast<char*>(name_var->data));

    //Matlab stores numeric values as doubles by default, which seems to be the type
    //used by TROTS as well even for integral values. Do some casting here to convert the data to
    //more intuitive types.
    matvar_t* id_var = Mat_VarGetStructFieldByName(problem_struct_entry, "dataID", 0);
    check_null(id_var, "Could not read id field from struct\n");
    this->id = cast_from_double<int>(id_var);


    matvar_t* minimise_var = Mat_VarGetStructFieldByName(problem_struct_entry, "Minimise", 0);
    check_null(minimise_var, "Could not read Minimise field from struct\n");
    this->minimise = cast_from_double<bool>(minimise_var);

    matvar_t* active_var = Mat_VarGetStructFieldByName(problem_struct_entry, "Active", 0);
    check_null(active_var, "Could not read Active field from struct\n");
    this->active = cast_from_double<bool>(active_var);

    //The IsConstraint field goes against the trend of using doubles, and is actually a MATLAB logical val,
    //which matio has as a MAT_C_UINT8
    matvar_t* is_cons_var = Mat_VarGetStructFieldByName(problem_struct_entry, "IsConstraint", 0);
    check_null(is_cons_var, "Could not read IsConstraint field from struct\n");
    this->is_cons = *static_cast<bool*>(is_cons_var->data);

    matvar_t* objective_var = Mat_VarGetStructFieldByName(problem_struct_entry, "Objective", 0);
    check_null(objective_var, "Could not read Objective field from struct\n");
    this->rhs = *static_cast<double*>(objective_var->data);

    matvar_t* function_type = Mat_VarGetStructFieldByName(problem_struct_entry, "Type", 0);
    check_null(function_type, "Could not read the \"Type\" field from the problem struct\n");
    int TROTS_type = cast_from_double<int>(function_type);
    //An index of 1 means a "linear" function, which in reality can be one of three possibilities. Min, max and mean.
    //Determine which one it is.
    if (TROTS_type == 1)
        this->type = get_linear_function_type(this->id, this->minimise, this->roi_name, matrix_struct);
    else
        this->type = get_nonlinear_function_type(TROTS_type);

    if (this->type == FunctionType::Mean) {
        this->matrix_ref = nullptr;
        this->mean_vec_ref = &std::get<std::vector<double>>(mat_refs[this->id - 1]);
    } else {
        this->mean_vec_ref = nullptr;
        this->matrix_ref = std::get<std::unique_ptr<SparseMatrix<double>>>(mat_refs[this->id - 1]).get();
    }

    matvar_t* weight_var = Mat_VarGetStructFieldByName(problem_struct_entry, "Weight", 0);
    check_null(weight_var, "Could not read the \"Weight\" field from the problem struct.\n");
    this->weight = *static_cast<double*>(weight_var->data);

    matvar_t* parameters_var = Mat_VarGetStructFieldByName(problem_struct_entry, "Parameters", 0);
    check_null(parameters_var, "Could not read the \"Parameters\" field from the problem struct.\n");
    const size_t num_elems = parameters_var->dims[0] * parameters_var->dims[1];
    if (num_elems > 0) {
        assert(parameters_var->dims[0] == 1); //The parameter array should be a row vector
        const double* elems = static_cast<double*>(parameters_var->data);
        for (int i = 0; i < num_elems; ++i) {
            this->func_params.push_back(*elems++);
        }
    }

    //A lot of the objective functions require a temporary y vector to hold the dose. To avoid allocating that space for each
    //call to compute the objective value, we pre-allocate storage for it in this->y_vec.
    if (this->type != FunctionType::Mean) {
        const auto num_rows = this->matrix_ref->get_rows();
        const auto num_cols = this->matrix_ref->get_cols();
        this->y_vec.resize(num_rows);
        this->grad_tmp.resize(num_rows);
        this->num_vars = num_cols;
    } else {
        const auto num_cols = this->mean_vec_ref->size();
        this->num_vars = num_cols;
    }

    if (this->type == FunctionType::Quadratic) {
        matvar_t* c_var = Mat_VarGetStructFieldByName(matrix_struct, "c", this->id - 1);
        assert(c_var->data_type == MAT_T_SINGLE);
        this->c = static_cast<double>(*static_cast<float*>(c_var->data));
    }

    this->grad_nonzero_idxs = this->calc_grad_nonzero_idxs();
    //Sanity check: the mean function type should have a collapsed dose matrix (i.e. a vector)
    //              other function types should have a matrix. Check that the other type is nullptr for each.
    assert((this->type == FunctionType::Mean && this->matrix_ref == nullptr)
        || (this->type != FunctionType::Mean && this->mean_vec_ref == nullptr));
}

std::vector<double> TROTSEntry::calc_sparse_grad(const double* x) const {
    std::vector<double> dense_grad(this->num_vars);
    this->calc_gradient(x, &dense_grad[0]);
    double sum = std::accumulate(dense_grad.cbegin(), dense_grad.cend(), 0.0);
    std::vector<double> sparse_grad;
    sparse_grad.reserve(this->grad_nonzero_idxs.size());
    for (int idx : this->grad_nonzero_idxs) {
        sparse_grad.push_back(dense_grad[idx]);
    }

    return sparse_grad;
}

double TROTSEntry::calc_value(const double* x) const {
    switch (this->type) {
        case FunctionType::Quadratic:
            return this->calc_quadratic(x);
        case FunctionType::Max:
            return this->quadratic_penalty_max(x);
        case FunctionType::Min:
            return this->quadratic_penalty_min(x);
        case FunctionType::Mean:
            return this->calc_mean(x);
        case FunctionType::gEUD:
            return this->calc_gEUD(x);
        case FunctionType::LTCP:
            return this->calc_LTCP(x);
        default:
            //throw "Not implemented yet!\n";
            return 0.0;
    }
}

double TROTSEntry::calc_quadratic(const double* x) const {
    return 0.5 * this->matrix_ref->quad_mul(x, &this->y_vec[0]) + this->c;
}

double TROTSEntry::calc_max(const double* x) const {
    this->matrix_ref->vec_mul(x, &this->y_vec[0]);

    const double max_elem = *std::max_element(this->y_vec.cbegin(), this->y_vec.cend());
    return max_elem;
}

double TROTSEntry::calc_min(const double* x) const {
    this->matrix_ref->vec_mul(x, &this->y_vec[0]);

    const double min_elem = *std::min_element(this->y_vec.cbegin(), this->y_vec.cend());
    return min_elem;
}

double TROTSEntry::calc_mean(const double* x) const {
    return cblas_ddot(this->mean_vec_ref->size(), x, 1, &(*this->mean_vec_ref)[0], 1);
}

double TROTSEntry::calc_LTCP(const double* x) const {
    this->matrix_ref->vec_mul(x, &this->y_vec[0]);

    const double prescribed_dose = this->func_params[0];
    const double alpha = this->func_params[1];
    double sum = 0.0;
    for (int i = 0; i < this->y_vec.size(); ++i) {
        sum += std::exp(-alpha * (this->y_vec[i] - prescribed_dose));
    }

    return sum / this->matrix_ref->get_rows();
}

double TROTSEntry::calc_gEUD(const double* x) const {
    this->matrix_ref->vec_mul(x, &this->y_vec[0]);
    const auto num_voxels = this->y_vec.size();

    const double a = this->func_params[0];
    double sum = 0.0;
    for (int i = 0; i < num_voxels; ++i) {
        sum += std::pow(this->y_vec[i], a);
    }
    const double val = std::pow(sum / static_cast<double>(num_voxels), 1/a);
    return val;
}

double TROTSEntry::quadratic_penalty_mean(const double* x) const {
    const double mean = cblas_ddot(this->mean_vec_ref->size(), x, 1, &(*this->mean_vec_ref)[0], 1);
    const double diff = this->minimise ?
                            std::min(mean - this->rhs, 0.0) :
                            std::max(mean - this->rhs, 0.0);
    return diff * diff;
}

double TROTSEntry::quadratic_penalty_min(const double* x) const {
    this->matrix_ref->vec_mul(x, &this->y_vec[0]);

    double sq_diff = 0.0;
    const size_t num_voxels = this->y_vec.size();
    for (int i = 0; i < num_voxels; ++i) {
        double clamped_diff = std::min(this->y_vec[i] - this->rhs, 0.0);
        sq_diff += clamped_diff * clamped_diff;
    }

    return sq_diff / static_cast<double>(num_voxels);
}

double TROTSEntry::quadratic_penalty_max(const double* x) const {
    this->matrix_ref->vec_mul(x, &this->y_vec[0]);

    double sq_diff = 0.0;
    const size_t num_voxels = this->y_vec.size();
    for (int i = 0; i < num_voxels; ++i) {
        double clamped_diff = std::max(this->y_vec[i] - this->rhs, 0.0);
        sq_diff += clamped_diff * clamped_diff;
    }

    return sq_diff / static_cast<double>(num_voxels);
}

void TROTSEntry::calc_gradient(const double* x, double* grad) const {
    switch (this->type) {
        case FunctionType::Quadratic:
            quad_grad(x, grad);
            break;
        case FunctionType::Max:
            quad_max_grad(x, grad, false);
            break;
        case FunctionType::Min:
            quad_min_grad(x, grad, false);
            break;
        case FunctionType::Mean:
            mean_grad(x, grad);
            break;
        case FunctionType::gEUD:
            gEUD_grad(x, grad, false);
            break;
        case FunctionType::LTCP:
            LTCP_grad(x, grad, false);
            break;
        default:
            //std::fill(grad, grad + this->num_vars, 0.0);
            break;
    }
}

std::vector<int> TROTSEntry::calc_grad_nonzero_idxs() const {
    std::vector<int> non_zeros;
    if (this->function_type() != FunctionType::Mean) {
        int cols = this->matrix_ref->get_cols();
        int nnz = this->matrix_ref->get_nnz();
        const int* col_inds = this->matrix_ref->get_col_inds();
        std::set<int> non_zero_cols;
        for (int i = 0; i < nnz; ++i) {
            non_zero_cols.insert(col_inds[i]);
        }
        non_zeros.reserve(non_zeros.size());
        //Convert to vector before returning
        std::copy(non_zero_cols.cbegin(), non_zero_cols.cend(), std::back_inserter(non_zeros));

    } else {
        //The gradient is just the "average vector" so find the nonzeros there
        for (int i = 0; i < this->mean_vec_ref->size(); ++i) {
            double entry = (*this->mean_vec_ref)[i];
            if (entry >= 1e-20) {
                non_zeros.push_back(i);
            }
        }
    }
    return non_zeros;
}

void TROTSEntry::mean_grad(const double* x, double* grad) const {
    std::copy(this->mean_vec_ref->cbegin(), this->mean_vec_ref->cend(), grad);
}

void TROTSEntry::LTCP_grad(const double* x, double* grad, bool cached_dose) const {
    const auto num_voxels = this->matrix_ref->get_rows();
    if (!cached_dose) {
        this->matrix_ref->vec_mul(x, &this->y_vec[0]);
    }

    const double prescribed_dose = this->func_params[0];
    const double alpha = this->func_params[1];
    for (int i = 0; i < this->grad_tmp.size(); ++i) {
        this->grad_tmp[i] = -alpha / num_voxels * std::exp(-alpha * (this->y_vec[i] - prescribed_dose));
    }

    this->matrix_ref->vec_mul_transpose(&this->grad_tmp[0], grad);
}

void TROTSEntry::gEUD_grad(const double* x, double* grad, bool cached_dose) const {
    const auto num_voxels = this->matrix_ref->get_rows();
    if (!cached_dose) {
        this->matrix_ref->vec_mul(x, &this->y_vec[0]);
    }
    const double a = this->func_params[0];

    //Calculate the factor that all entries have in common, namely m^a * (\sum d_i(x)^a)^(1/a - 1)
    double common_factor = 0.0;
    for (int i = 0; i < num_voxels; ++i) {
        common_factor += std::pow(this->y_vec[i], a);
    }
    common_factor = std::pow(common_factor, (1 / a) - 1);
    common_factor *= std::pow(num_voxels, -1/a);

    for (int i = 0; i < this->grad_tmp.size(); ++i) {
        this->grad_tmp[i] = std::pow(this->y_vec[i], a - 1) * common_factor;
    }

    this->matrix_ref->vec_mul_transpose(&this->grad_tmp[0], grad);
}

void TROTSEntry::quad_min_grad(const double* x, double* grad, bool cached_dose) const {
    //Sometimes, this->y_vec will already contain the current dose vector, no need to recompute in this case
    if (!cached_dose) {
        this->matrix_ref->vec_mul(x, &this->y_vec[0]);
    }

    for (int i = 0; i < this->grad_tmp.size(); ++i) {
        this->grad_tmp[i] = 2 * std::min(this->y_vec[i] - this->rhs, 0.0);
    }

    this->matrix_ref->vec_mul_transpose(&this->grad_tmp[0], grad);
}

void TROTSEntry::quad_max_grad(const double* x, double* grad, bool cached_dose) const {
    //Sometimes, this->y_vec will already contain the current dose vector, no need to recompute in this case
    if (!cached_dose) {
        this->matrix_ref->vec_mul(x, &this->y_vec[0]);
    }

    for (int i = 0; i < this->grad_tmp.size(); ++i) {
        grad_tmp[i] = 2 * std::max(this->y_vec[i] - this->rhs, 0.0);
    }

    this->matrix_ref->vec_mul_transpose(&this->grad_tmp[0], grad);
}

void TROTSEntry::quad_grad(const double* x, double* grad) const {
    this->matrix_ref->vec_mul(x, grad);
}
