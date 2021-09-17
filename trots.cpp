#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>

#include "trots.h"
#include "util.h"
namespace fs = std::filesystem;

namespace {
    std::vector<double> get_mean_vector(matvar_t* matrix_entry) {
        matvar_t* matrix_data_var = Mat_VarGetStructFieldByName(matrix_entry, "A", 0);
        check_null(matrix_data_var, "Could not read matrix A from struct.\n");
        assert(matrix_data_var->dims[0] == 1); //Should be a pure column vector
        size_t num_elems = matrix_data_var->dims[1];
        
        std::vector<double> A;
        A.reserve(num_elems);
        double* data = static_cast<double*>(matrix_data_var->data);
        for (size_t i = 0; i < num_elems; ++i)
            A.push_back(data[i]);
        
        return A;
    }

    //Reads the Matlab sparse matrix stored at idx dataID - 1 and converts it to a CSR format and MKL sparse type.
    MKL_sparse_matrix<double> read_and_cvt_sparse_mat(matvar_t* matrix_entry) {
        matvar_t* matrix_data_var = Mat_VarGetStructFieldByName(matrix_entry, "A", 0);
        check_null(matrix_data_var, "Could not read matrix A from struct.\n");
        assert(matrix_data_var->class_type == MAT_C_SPARSE);

        mat_sparse_t* matlab_sparse_m = static_cast<mat_sparse_t*>(matrix_data_var->data);
        const int nnz = static_cast<int>(matlab_sparse_m->ndata);
        const int rows = static_cast<int>(matrix_data_var->dims[0]);
        const int cols = static_cast<int>(matrix_data_var->dims[1]);

        return MKL_sparse_matrix<double>::from_CSC_mat(nnz, rows, cols,
                                                       static_cast<double*>(matlab_sparse_m->data),
                                                       matlab_sparse_m->ir,
                                                       matlab_sparse_m->jc);
    }
}

TROTSMatFileData::TROTSMatFileData(const fs::path& path) {
    this->file_fp = Mat_Open(path.c_str(), MAT_ACC_RDONLY);
    this->init_problem_data_structs();
}

TROTSMatFileData::TROTSMatFileData(TROTSMatFileData&& other) {
    this->file_fp = other.file_fp;
    this->data_struct = other.data_struct;
    this->problem_struct = other.problem_struct;
    this->matrix_struct = other.matrix_struct;

    other.file_fp = NULL;
    other.data_struct = NULL;
    other.problem_struct = NULL;
    other.matrix_struct = NULL;
}

TROTSMatFileData::~TROTSMatFileData() {
    std::cerr << "Destroying MatFileData...\n";
    Mat_VarFree(this->data_struct);
    Mat_Close(this->file_fp);
}

void TROTSMatFileData::init_problem_data_structs() {
    this->problem_struct = Mat_VarRead(this->file_fp, "problem");
    check_null(this->problem_struct, "Unable to read problem struct from matfile\n.");
    
    this->data_struct = Mat_VarRead(this->file_fp, "data");
    check_null(this->data_struct, "Unable to read data variable from matfile\n");

    this->matrix_struct = Mat_VarGetStructFieldByName(this->data_struct, "matrix", 0);
    check_null(this->matrix_struct, "Unable to read matrix field from matfile\n");
}

TROTSProblem::TROTSProblem(TROTSMatFileData&& trots_data_) :
    trots_data{std::move(trots_data_)}
{
    matvar_t* problem_struct = this->trots_data.problem_struct;
    size_t num_entries = problem_struct->dims[1];

    this->read_dose_matrices();

    int stride[] = {0, 0};
    int edge[] = {1, 1};
    for (int i = 0; i < num_entries; ++i) {
        std::cerr << "Reading trots entry " << i << " of " << num_entries << "...\n";
        int start[] =  {0, i};
        matvar_t* struct_elem = Mat_VarGetStructs(problem_struct, start, stride, edge, 0);
        const TROTSEntry entry{struct_elem, this->trots_data.matrix_struct, this->matrices};
        std::cerr << "TROTSEntry read!\n\n";
        if (entry.is_constraint())
            this->constraint_entries.push_back(entry);
        else
            this->objective_entries.push_back(entry );
    }

    matvar_t* misc_struct = Mat_VarGetStructFieldByName(this->trots_data.data_struct, "misc", 0);
    matvar_t* size_var = Mat_VarGetStructFieldByName(misc_struct, "size", 0);
    this->num_vars = cast_from_double<int>(size_var);
    std::cerr << "Num vars: " << this->num_vars << "\n";
}

void TROTSProblem::read_dose_matrices() {
    int stride[] = {0, 0};
    int edge[] = {1, 1};

    size_t num_matrices = this->trots_data.matrix_struct->dims[1];
    this->matrices.reserve(num_matrices);
    for (int i = 0; i < num_matrices; ++i) {
        std::cerr << "Reading dose matrix " << i + 1 << " of " << num_matrices << "...\n";
        int start[] = {0, i};
        matvar_t* matrix_entry = Mat_VarGetStructs(this->trots_data.matrix_struct, start, stride, edge, 0);
        check_null(matrix_entry, "Failed to read entry " + std::to_string(i) + " from matrix.data\n");
        matvar_t* A = Mat_VarGetStructFieldByName(matrix_entry, "A", 0);
        check_null(A, "Failed to read A from entry " + std::to_string(i) + " in matrix.data\n");
        auto& new_variant = this->matrices.emplace_back();
        
        //For the mean functions, the "A"-matrix is reduced to a dense vector.
        //Check if we have a sparse matrix or dense vector
        if (A->class_type == MAT_C_SPARSE) {
            std::cerr << "Sparse mat found!\n";
            new_variant.emplace<MKL_sparse_matrix<double>>(
                read_and_cvt_sparse_mat(matrix_entry)
            );
        }
        else {
            std::cerr << "Dense vector found!\n";
            new_variant.emplace<std::vector<double>>(
                get_mean_vector(matrix_entry)
            );
        }
        
        //Avoid storing the matrix data twice.
        Mat_VarFree(A);

        //To avoid double free later, we should set this entry in the matrix struct to some placeholder value
        matvar_t* placeholder = Mat_VarCalloc();
        Mat_VarSetStructFieldByName(matrix_entry, "A", 0, placeholder);
    }
}

