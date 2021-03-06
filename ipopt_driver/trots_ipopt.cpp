#include "trots_ipopt.h"
#include "util.h"

#include "coin-or/IpIpoptApplication.hpp"

namespace {
    int max_iter = 20000;

    void calc_values_test(const Ipopt::SmartPtr<Ipopt::TNLP>& tnlp, int n, int m) {
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
}

TROTS_ipopt::TROTS_ipopt(TROTSProblem&& problem) {
    this->problem = std::make_unique<TROTSProblem>(std::move(problem));
}

bool TROTS_ipopt::get_nlp_info(
    int& n, int& m, int& nnz_jac_g, int& nnz_h_lag,
    Ipopt::TNLP::IndexStyleEnum& index_style) {

    n = this->problem->get_num_vars();
    m = this->problem->get_num_constraints();

    nnz_jac_g = this->problem->get_nnz_jac_cons();
    nnz_h_lag = n * n / 2; //Dense but symmetric
    index_style = C_STYLE;

    return true;
}


bool TROTS_ipopt::get_bounds_info(int n, double* x_l, double* x_u, int m, double* g_l, double* g_u) {
    //By default, IPOPT considers values greater than 1e19 in magnitude as infinite.
    constexpr double neg_inf = -1e20;
    constexpr double pos_inf = 1e20;
    for (int i = 0; i < n; ++i) {
        x_l[i] = 0.0;
        x_u[i] = pos_inf;
    }

    for (int i = 0; i < m; ++i) {
        const TROTSEntry& cons_entry = this->problem->constraint_entries[i];
        if (cons_entry.is_minimisation() ||
            cons_entry.function_type() == FunctionType::Min ||
            cons_entry.function_type() == FunctionType::Max) {
            g_l[i] = neg_inf;
            g_u[i] = 0.0;
        } else {
            g_l[i] = 0.0;
            g_u[i] = pos_inf;
        }
    }

    return true;
}


bool TROTS_ipopt::get_starting_point(
    int n, bool init_x, double* x, bool init_z, double* z_l, double* z_u,
    int m, bool init_lambda, double* lambda) {

    if (init_x) {
        //The initialization of the primal variables is based on the goal of
        //making the LTCP objectives not too large to start with.
        //We use the "simple" initialization strategy described in
        //https://doi.org/10.1007/s10589-017-9919-4

        std::vector<TROTSEntry> LTCP_entries;
        std::copy_if(this->problem->objective_entries.cbegin(), this->problem->objective_entries.cend(),
                     std::back_inserter(LTCP_entries),
                     [](const TROTSEntry& e){ return e.function_type() == FunctionType::LTCP; });

        for (int i = 0; i < n; ++i) {
            x[i] = 100.0;
        }

        const auto compute_obj_val = [x](const TROTSEntry& e) -> double {
            return e.calc_value(x);
        };
        std::vector<double> LTCP_vals(LTCP_entries.size());
        std::transform(LTCP_entries.cbegin(), LTCP_entries.cend(),
                       LTCP_vals.begin(), compute_obj_val);
        while (std::any_of(LTCP_vals.cbegin(), LTCP_vals.cend(), [](double v){ return v > 1500.0; })) {
            for (int i = 0; i < n; ++i) {
                x[i] *= 1.5;
            }
            std::transform(LTCP_entries.cbegin(), LTCP_entries.cend(),
                           LTCP_vals.begin(), compute_obj_val);
        }
        std::cout << "Initial x: " << x[0] << std::endl;
    }

    //We have no bounds, so these probably don't matter, set them to zero in case.
    if (init_z) {
        for (int i = 0; i < n; ++i) {
            z_l[i] = 0.0;
            z_u[i] = 0.0;
        }
    }

    if (init_lambda) {
        //TODO: smarter initalization
        for (int i = 0; i < m; ++i) {
            lambda[i] = 1.0;
        }
    }

    return true;
}

bool TROTS_ipopt::eval_f(int n, const double* x, bool new_x, double& obj_val) {
    obj_val = this->problem->calc_objective(x);
    return true;
}

bool TROTS_ipopt::eval_grad_f(int n, const double* x, bool new_x, double* grad_f) {
    this->problem->calc_obj_gradient(x, grad_f);
    return true;
}

bool TROTS_ipopt::eval_g(int n, const double* x, bool new_x, int m, double* g) {
    this->problem->calc_constraints(x, g);
    return true;
}

bool TROTS_ipopt::eval_jac_g(int n, const double* x, bool new_x,
                             int m, int nnz_jac, int* irow, int* icol, double* vals) {

    if (vals == nullptr) {
        assert(irow != nullptr && icol != nullptr);
        int i = 0;
        for (int row = 0; row < this->problem->get_num_constraints(); ++row) {
            std::vector<int> cols = this->problem->constraint_entries[row].get_grad_nonzero_idxs();
            for (int col : cols) {
                irow[i] = row;
                icol[i] = col;
                ++i;
            }
        }
        return true;
    }

    this->problem->calc_jacobian_vals(x, vals);
    return true;
}


void TROTS_ipopt::finalize_solution(Ipopt::SolverReturn status, int n,
    const double* x, const double* z_l, const double* z_u,
    int m, const double* g, const double* lambda, double obj,
    const Ipopt::IpoptData* ip_data, Ipopt::IpoptCalculatedQuantities* ip_cq) {

    //Output to file: reuse code for dumping std::vector arrays to file
    std::vector<double> x_vec;
    x_vec.reserve(n);
    for (int i = 0; i < n; ++i) {
        x_vec.push_back(x[i]);
    }

    dump_vector_to_file(x_vec, "mod_rhs_new.bin");

    std::cout << "IPOPT finalize_solution called\n";
    std::cout << "Exit status: " << status << "\n";
}

int ipopt_main_func(int argc, char* argv[]) {
    if (argc < 2 || argc > 3)  {
        std::cerr << "Incorrect number of arguments\n";
        std::cerr << "Usage: ./program <mat_file_path>\n";
        std::cerr << "\t./program <mat_file_path> <max_iters>\n";
        return -1;
    }

    std::string path_str{argv[1]};
    std::filesystem::path path{path_str};

    if (argc == 3) {
        max_iter = std::atoi(argv[2]);
    }

    TROTSProblem trots_problem{TROTSMatFileData{path}};
    const int n = trots_problem.get_num_vars();
    const int m = trots_problem.get_num_constraints();
    Ipopt::SmartPtr<Ipopt::TNLP> trots_nlp = new TROTS_ipopt(std::move(trots_problem));
    calc_values_test(trots_nlp, n, m);
    Ipopt::SmartPtr<Ipopt::IpoptApplication> app = IpoptApplicationFactory();
    app->Options()->SetStringValue("hessian_approximation", "limited-memory");
    app->Options()->SetStringValue("mu_strategy", "adaptive");
    app->Options()->SetStringValue("adaptive_mu_globalization", "kkt-error");
    app->Options()->SetIntegerValue("max_iter", max_iter);
    app->Options()->SetNumericValue("tol", 1e-9);
    //app->Options()->SetStringValue("print_timing_statistics", "yes");
    //app->Options()->SetStringValue("derivative_test", "first-order");
    //app->Options()->SetNumericValue("derivative_test_perturbation", 1e-12);

    Ipopt::ApplicationReturnStatus status;
    status = app->Initialize();
    status = app->OptimizeTNLP(trots_nlp);

    return status;
}
