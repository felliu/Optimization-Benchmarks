#ifndef TROTS_H
#define TROTS_H

#include <filesystem>
#include <string>
#include <cassert>
#include <variant>
#include <vector>

#include <matio.h>
#include <mkl.h>


#include "OptimizationProblem.h"
#include "SparseMat.h"
#include "TROTSEntry.h"

struct TROTSMatFileData {
public:
    TROTSMatFileData(const std::filesystem::path& file_path);

    mat_t* file_fp;
    matvar_t* problem_struct;
    matvar_t* data_struct;
    matvar_t* matrix_struct;
private:
    void init_problem_data_structs();
};

class TROTSProblem { //: public OptimizationProblem {
public:
    TROTSProblem(const TROTSMatFileData& trots_data);
private:
    void read_dose_matrices();

    TROTSMatFileData trots_data;
    std::vector<TROTSEntry> objective_entries;
    std::vector<TROTSEntry> constraint_entries;
    
    //List of matrix entries, indexed by dataID.
    //If the FunctionType is mean, the value is computed using a dot product with a dense vector,
    //In other cases, the dose is calculated using a dose deposition matrix.
    std::vector<std::variant<MKL_sparse_matrix<double>, std::vector<double>>> matrices;
};

#endif