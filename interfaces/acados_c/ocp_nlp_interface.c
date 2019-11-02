/*
 * Copyright 2019 Gianluca Frison, Dimitris Kouzoupis, Robin Verschueren,
 * Andrea Zanelli, Niels van Duijkeren, Jonathan Frey, Tommaso Sartor,
 * Branimir Novoselnik, Rien Quirynen, Rezart Qelibari, Dang Doan,
 * Jonas Koenemann, Yutao Chen, Tobias Schöls, Jonas Schlagenhauf, Moritz Diehl
 *
 * This file is part of acados.
 *
 * The 2-Clause BSD License
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.;
 */


#include "acados_c/ocp_nlp_interface.h"

// external
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "acados/ocp_nlp/ocp_nlp_common.h"
#include "acados/ocp_nlp/ocp_nlp_cost_external.h"
#include "acados/ocp_nlp/ocp_nlp_cost_ls.h"
#include "acados/ocp_nlp/ocp_nlp_cost_nls.h"
#include "acados/ocp_nlp/ocp_nlp_dynamics_cont.h"
#include "acados/ocp_nlp/ocp_nlp_dynamics_disc.h"
#include "acados/ocp_nlp/ocp_nlp_constraints_bgh.h"
#include "acados/ocp_nlp/ocp_nlp_constraints_bgp.h"
#include "acados/ocp_nlp/ocp_nlp_reg_convexify.h"
#include "acados/ocp_nlp/ocp_nlp_reg_mirror.h"
#include "acados/ocp_nlp/ocp_nlp_reg_project.h"
#include "acados/ocp_nlp/ocp_nlp_reg_project_reduc_hess.h"
#include "acados/ocp_nlp/ocp_nlp_reg_noreg.h"
#include "acados/ocp_nlp/ocp_nlp_sqp.h"
#include "acados/ocp_nlp/ocp_nlp_sqp_rti.h"
#include "acados/utils/mem.h"


/************************************************
* plan
************************************************/

static int ocp_nlp_plan_calculate_size(int N)
{
    // N - number of shooting nodes
    int bytes = sizeof(ocp_nlp_plan);
    bytes += N * sizeof(sim_solver_plan);
    bytes += (N + 1) * sizeof(ocp_nlp_cost_t);
    bytes += N * sizeof(ocp_nlp_dynamics_t);
    bytes += (N+1) * sizeof(ocp_nlp_constraints_t);
    return bytes;
}



static ocp_nlp_plan *ocp_nlp_plan_assign(int N, void *raw_memory)
{
    char *c_ptr = (char *) raw_memory;

    ocp_nlp_plan *plan = (ocp_nlp_plan *) c_ptr;
    c_ptr += sizeof(ocp_nlp_plan);

    plan->sim_solver_plan = (sim_solver_plan *) c_ptr;
    c_ptr += N * sizeof(sim_solver_plan);

    plan->nlp_cost = (ocp_nlp_cost_t *) c_ptr;
    c_ptr += (N + 1) * sizeof(ocp_nlp_cost_t);

    plan->nlp_dynamics = (ocp_nlp_dynamics_t *) c_ptr;
    c_ptr += N * sizeof(ocp_nlp_dynamics_t);

    plan->nlp_constraints = (ocp_nlp_constraints_t *) c_ptr;
    c_ptr += (N + 1) * sizeof(ocp_nlp_constraints_t);

    plan->N = N;

    return plan;
}



static void ocp_nlp_plan_initialize_default(ocp_nlp_plan *plan)
{
    int ii;

    int N = plan->N;

    // initialize to default value !=0 to detect empty plans
    // cost
    for (ii=0; ii <= N; ii++)
    {
        plan->nlp_cost[ii] = INVALID_COST;
    }
    // dynamics
    for (ii=0; ii < N; ii++)
    {
        plan->nlp_dynamics[ii] = INVALID_DYNAMICS;
    }
    // constraints
    for (ii=0; ii <= N; ii++)
    {
        plan->nlp_constraints[ii] = INVALID_CONSTRAINT;
    }
    // nlp solver
    plan->nlp_solver = INVALID_NLP_SOLVER;
    // qp solver
    plan->ocp_qp_solver_plan.qp_solver = INVALID_QP_SOLVER;
    // sim solver
    for (ii = 0; ii < N; ii++)
    {
        plan->sim_solver_plan[ii].sim_solver = INVALID_SIM_SOLVER;
    }

    // regularization: no reg by default
    plan->regularization = NO_REGULARIZE;


    return;
}



ocp_nlp_plan *ocp_nlp_plan_create(int N)
{
    int bytes = ocp_nlp_plan_calculate_size(N);
    void *ptr = acados_malloc(bytes, 1);

    ocp_nlp_plan *plan = ocp_nlp_plan_assign(N, ptr);

    ocp_nlp_plan_initialize_default(plan);

    return plan;
}



void ocp_nlp_plan_destroy(void* plan_)
{
    free(plan_);
}



/************************************************
* config
************************************************/

ocp_nlp_config *ocp_nlp_config_create(ocp_nlp_plan plan)
{
    int N = plan.N;

    /* calculate_size & malloc & assign */

    int bytes = ocp_nlp_config_calculate_size(N);
    void *config_mem = acados_calloc(1, bytes);
    ocp_nlp_config *config = ocp_nlp_config_assign(N, config_mem);

    /* initialize config according plan */

    // NLP solver
    switch (plan.nlp_solver)
    {
        case SQP:
            ocp_nlp_sqp_config_initialize_default(config);
            break;
        case SQP_RTI:
            ocp_nlp_sqp_rti_config_initialize_default(config);
            break;
        case INVALID_NLP_SOLVER:
            break;
            printf("\nerror: ocp_nlp_config_create: forgot to initialize plan->nlp_solver\n");
            exit(1);
            break;
        default:
            printf("\nerror: ocp_nlp_config_create: unsupported plan->nlp_solver\n");
            exit(1);
    }

    // QP solver
    ocp_qp_xcond_solver_config_initialize_from_plan(plan.ocp_qp_solver_plan.qp_solver, config->qp_solver);

    // regularization
    switch (plan.regularization)
    {
        case NO_REGULARIZE:
            ocp_nlp_reg_noreg_config_initialize_default(config->regularize);
            break;
        case MIRROR:
            ocp_nlp_reg_mirror_config_initialize_default(config->regularize);
            break;
        case PROJECT:
            ocp_nlp_reg_project_config_initialize_default(config->regularize);
            break;
        case PROJECT_REDUC_HESS:
            ocp_nlp_reg_project_reduc_hess_config_initialize_default(config->regularize);
            break;
        case CONVEXIFY:
            ocp_nlp_reg_convexify_config_initialize_default(config->regularize);
            break;
        default:
            printf("\nerror: ocp_nlp_config_create: unsupported plan->regularization\n");
            exit(1);
    }

    // cost
    for (int i = 0; i <= N; ++i)
    {
        switch (plan.nlp_cost[i])
        {
            case LINEAR_LS:
                ocp_nlp_cost_ls_config_initialize_default(config->cost[i]);
                break;
            case NONLINEAR_LS:
                ocp_nlp_cost_nls_config_initialize_default(config->cost[i]);
                break;
            case EXTERNALLY_PROVIDED:
                ocp_nlp_cost_external_config_initialize_default(config->cost[i]);
                break;
            case INVALID_COST:
                printf("\nerror: ocp_nlp_config_create: forgot to initialize plan->nlp_cost\n");
                exit(1);
                break;
            default:
                printf("\nerror: ocp_nlp_config_create: unsupported plan->nlp_cost\n");
                exit(1);
        }
    }

    // Dynamics
    for (int i = 0; i < N; ++i)
    {
        switch (plan.nlp_dynamics[i])
        {
            case CONTINUOUS_MODEL:
                ocp_nlp_dynamics_cont_config_initialize_default(config->dynamics[i]);
//                config->dynamics[i]->sim_solver = sim_config_create(plan.sim_solver[i]);
                sim_solver_t solver_name = plan.sim_solver_plan[i].sim_solver;

                switch (solver_name)
                {
                    case ERK:
                        sim_erk_config_initialize_default(config->dynamics[i]->sim_solver);
                        break;
                    case IRK:
                        sim_irk_config_initialize_default(config->dynamics[i]->sim_solver);
                        break;
                    case GNSF:
                        sim_gnsf_config_initialize_default(config->dynamics[i]->sim_solver);
                        break;
                    case LIFTED_IRK:
                        sim_lifted_irk_config_initialize_default(config->dynamics[i]->sim_solver);
                        break;
                    default:
                        printf("\nerror: ocp_nlp_config_create: unsupported plan->sim_solver\n");
                        exit(1);
                }

                break;
            case DISCRETE_MODEL:
                ocp_nlp_dynamics_disc_config_initialize_default(config->dynamics[i]);
                break;
            case INVALID_DYNAMICS:
                printf("\nerror: ocp_nlp_config_create: forgot to initialize plan->nlp_dynamics\n");
                exit(1);
                break;
            default:
                printf("\nerror: ocp_nlp_config_create: unsupported plan->nlp_dynamics\n");
                exit(1);
        }
    }

    // Constraints
    for (int i = 0; i <= N; ++i)
    {
        switch (plan.nlp_constraints[i])
        {
            case BGH:
                ocp_nlp_constraints_bgh_config_initialize_default(config->constraints[i]);
                break;
            case BGP:
                ocp_nlp_constraints_bgp_config_initialize_default(config->constraints[i]);
                break;
            case INVALID_CONSTRAINT:
                printf("\nerror: ocp_nlp_config_create: forgot to initialize plan->nlp_constraints\n");
                exit(1);
                break;
            default:
                printf("\nerror: ocp_nlp_config_create: unsupported plan->nlp_constraints\n");
                exit(1);
        }
    }

    return config;
}



void ocp_nlp_config_destroy(void *config_)
{
    free(config_);
}


/************************************************
* dims
************************************************/

ocp_nlp_dims *ocp_nlp_dims_create(void *config_)
{
    ocp_nlp_config *config = config_;

    int bytes = ocp_nlp_dims_calculate_size(config);

    void *ptr = acados_calloc(1, bytes);

    ocp_nlp_dims *dims = ocp_nlp_dims_assign(config, ptr);

    return dims;
}



void ocp_nlp_dims_destroy(void *dims_)
{
    free(dims_);
}



/************************************************
* NLP inputs
************************************************/

ocp_nlp_in *ocp_nlp_in_create(ocp_nlp_config *config, ocp_nlp_dims *dims)
{
    int bytes = ocp_nlp_in_calculate_size(config, dims);

    void *ptr = acados_calloc(1, bytes);

    ocp_nlp_in *nlp_in = ocp_nlp_in_assign(config, dims, ptr);

    return nlp_in;
}



void ocp_nlp_in_destroy(void *in)
{
    free(in);
}



void ocp_nlp_in_set(ocp_nlp_config *config, ocp_nlp_dims *dims, ocp_nlp_in *in, int stage,
        const char *field, void *value)
{
    int ii;

    int N = dims->N;

    if (!strcmp(field, "Ts"))
    {
        double *Ts_values = value;
        for (ii=0; ii<N; ii++)
            in->Ts[ii] = *Ts_values;
    }
    else
    {
        printf("\nerror: ocp_nlp_in_set: field %s not available\n", field);
        exit(1);
    }
    return;
}



int ocp_nlp_dynamics_model_set(ocp_nlp_config *config, ocp_nlp_dims *dims, ocp_nlp_in *in,
        int stage, const char *field, void *value)
{
    ocp_nlp_dynamics_config *dynamics_config = config->dynamics[stage];

    dynamics_config->model_set(dynamics_config, dims->dynamics[stage], in->dynamics[stage], field, value);

    return ACADOS_SUCCESS;
}



int ocp_nlp_cost_model_set(ocp_nlp_config *config, ocp_nlp_dims *dims,
        ocp_nlp_in *in, int stage, const char *field, void *value)
{
    ocp_nlp_cost_config *cost_config = config->cost[stage];

    return cost_config->model_set(cost_config, dims->cost[stage], in->cost[stage], field, value);

}



int ocp_nlp_constraints_model_set(ocp_nlp_config *config, ocp_nlp_dims *dims,
        ocp_nlp_in *in, int stage, const char *field, void *value)
{
    ocp_nlp_constraints_config *constr_config = config->constraints[stage];

    return constr_config->model_set(constr_config, dims->constraints[stage],
            in->constraints[stage], field, value);
}



/************************************************
* out
************************************************/

ocp_nlp_out *ocp_nlp_out_create(ocp_nlp_config *config, ocp_nlp_dims *dims)
{
    int bytes = ocp_nlp_out_calculate_size(config, dims);

    void *ptr = acados_calloc(1, bytes);

    ocp_nlp_out *nlp_out = ocp_nlp_out_assign(config, dims, ptr);

    // initialize to zeros
//    for (int ii = 0; ii <= dims->N; ++ii)
//        blasfeo_dvecse(dims->qp_solver->nu[ii] + dims->qp_solver->nx[ii], 0.0, nlp_out->ux + ii, 0);

    return nlp_out;
}



void ocp_nlp_out_destroy(void *out)
{
    free(out);
}



void ocp_nlp_out_set(ocp_nlp_config *config, ocp_nlp_dims *dims, ocp_nlp_out *out,
        int stage, const char *field, void *value)
{
    if (!strcmp(field, "x"))
    {
        double *double_values = value;
        blasfeo_pack_dvec(dims->nx[stage], double_values, &out->ux[stage], dims->nu[stage]);
    }
    else if (!strcmp(field, "u"))
    {
        double *double_values = value;
        blasfeo_pack_dvec(dims->nu[stage], double_values, &out->ux[stage], 0);
    }
    else if (!strcmp(field, "pi"))
    {
        double *double_values = value;
        blasfeo_pack_dvec(dims->nx[stage+1], double_values, &out->pi[stage], 0);
    }
    else
    {
        printf("\nerror: ocp_nlp_out_set: field %s not available\n", field);
        exit(1);
    }
}



void ocp_nlp_out_get(ocp_nlp_config *config, ocp_nlp_dims *dims, ocp_nlp_out *out,
        int stage, const char *field, void *value)
{
    if (!strcmp(field, "x"))
    {
        double *double_values = value;
        blasfeo_unpack_dvec(dims->nx[stage], &out->ux[stage], dims->nu[stage], double_values);
    }
    else if (!strcmp(field, "u"))
    {
        double *double_values = value;
        blasfeo_unpack_dvec(dims->nu[stage], &out->ux[stage], 0, double_values);
    }
    else if (!strcmp(field, "z"))
    {
        double *double_values = value;
        blasfeo_unpack_dvec(dims->nz[stage], &out->z[stage], 0, double_values);
    }
    else if (!strcmp(field, "pi"))
    {
        double *double_values = value;
        blasfeo_unpack_dvec(dims->nx[stage+1], &out->pi[stage], 0, double_values);
    }
    else if ((!strcmp(field, "kkt_norm_inf")) || (!strcmp(field, "kkt_norm")))
    {
        double *double_values = value;
        double_values[0] = out->inf_norm_res;
    }
    else
    {
        printf("\nerror: ocp_nlp_out_get: field %s not available\n", field);
        exit(1);
    }
}



int ocp_nlp_dims_get_from_attr(ocp_nlp_config *config, ocp_nlp_dims *dims, ocp_nlp_out *out,
        int stage, const char *field)
{
    int dims_value = -1;

    // ocp_nlp_dims
    if (!strcmp(field, "x"))
    {
        return dims->nx[stage];
    }
    else if (!strcmp(field, "u"))
    {
        return dims->nu[stage];
    }
    else if (!strcmp(field, "z"))
    {
        return dims->nz[stage];
    }
    // ocp_nlp_constraints_dims
    else if (!strcmp(field, "lbx") || !strcmp(field, "ubx"))
    {
        config->constraints[stage]->dims_get(config->constraints[stage], dims->constraints[stage],
                                            "nbx", &dims_value);
        return dims_value;
    }
    else if (!strcmp(field, "lbu") || !strcmp(field, "ubu"))
    {
        config->constraints[stage]->dims_get(config->constraints[stage], dims->constraints[stage],
                                            "nbu", &dims_value);
        return dims_value;
    }
    else if (!strcmp(field, "lg") || !strcmp(field, "ug"))
    {
        config->constraints[stage]->dims_get(config->constraints[stage], dims->constraints[stage],
                                            "ng", &dims_value);
        return dims_value;
    }
    else if (!strcmp(field, "s"))
    {
        config->constraints[stage]->dims_get(config->constraints[stage], dims->constraints[stage],
                                            "ns", &dims_value);
        return dims_value;
    }
    // ocp_nlp_cost_dims
    else if (!strcmp(field, "y_ref") || !strcmp(field, "yref"))
    {
        config->cost[stage]->dims_get(config->cost[stage], dims->cost[stage],
                                            "ny", &dims_value);
        return dims_value;
    }
    else
    {
        printf("\nerror: ocp_nlp_dims_get: field %s not available\n", field);
        exit(1);
    }
}


/************************************************
* opts
************************************************/

void *ocp_nlp_solver_opts_create(ocp_nlp_config *config, ocp_nlp_dims *dims)
{
    int bytes = config->opts_calculate_size(config, dims);

    void *ptr = acados_calloc(1, bytes);

    void *opts = config->opts_assign(config, dims, ptr);

    config->opts_initialize_default(config, dims, opts);

    return opts;
}



void ocp_nlp_solver_opts_set(ocp_nlp_config *config, void *opts_, const char *field, void *value)
{
    config->opts_set(config, opts_, field, value);
}



void ocp_nlp_solver_opts_set_at_stage(ocp_nlp_config *config, void *opts_, int stage, const char *field, void *value)
{
    config->opts_set_at_stage(config, opts_, stage, field, value);
}



void ocp_nlp_solver_opts_update(ocp_nlp_config *config, ocp_nlp_dims *dims, void *opts_)
{
    config->opts_update(config, dims, opts_);
}



void ocp_nlp_solver_opts_destroy(void *opts)
{
    free(opts);
}



/************************************************
* solver
************************************************/

static int ocp_nlp_calculate_size(ocp_nlp_config *config, ocp_nlp_dims *dims, void *opts_)
{
    int bytes = sizeof(ocp_nlp_solver);

    bytes += config->memory_calculate_size(config, dims, opts_);
    bytes += config->workspace_calculate_size(config, dims, opts_);

    return bytes;
}



static ocp_nlp_solver *ocp_nlp_assign(ocp_nlp_config *config, ocp_nlp_dims *dims,
                                      void *opts_, void *raw_memory)
{
    char *c_ptr = (char *) raw_memory;

    ocp_nlp_solver *solver = (ocp_nlp_solver *) c_ptr;
    c_ptr += sizeof(ocp_nlp_solver);

    solver->config = config;
    solver->dims = dims;
    solver->opts = opts_;

    solver->mem = config->memory_assign(config, dims, opts_, c_ptr);
    // printf("\nsolver->mem %p", solver->mem);
    c_ptr += config->memory_calculate_size(config, dims, opts_);

    solver->work = (void *) c_ptr;
    c_ptr += config->workspace_calculate_size(config, dims, opts_);

    assert((char *) raw_memory + ocp_nlp_calculate_size(config, dims, opts_) == c_ptr);

    return solver;
}



ocp_nlp_solver *ocp_nlp_solver_create(ocp_nlp_config *config, ocp_nlp_dims *dims, void *opts_)
{
    config->opts_update(config, dims, opts_);

    int bytes = ocp_nlp_calculate_size(config, dims, opts_);

    void *ptr = acados_calloc(1, bytes);

    ocp_nlp_solver *solver = ocp_nlp_assign(config, dims, opts_, ptr);

    return solver;
}


void ocp_nlp_solver_destroy(void *solver)
{
    free(solver);
}



int ocp_nlp_solve(ocp_nlp_solver *solver, ocp_nlp_in *nlp_in, ocp_nlp_out *nlp_out)
{
    return solver->config->evaluate(solver->config, solver->dims, nlp_in, nlp_out,
                                    solver->opts, solver->mem, solver->work);
}



int ocp_nlp_precompute(ocp_nlp_solver *solver, ocp_nlp_in *nlp_in, ocp_nlp_out *nlp_out)
{
    return solver->config->precompute(solver->config, solver->dims, nlp_in, nlp_out,
                                      solver->opts, solver->mem, solver->work);
}



void ocp_nlp_eval_param_sens(ocp_nlp_solver *solver, char *field, int stage, int index,
                             ocp_nlp_out *sens_nlp_out)
{
    solver->config->eval_param_sens(solver->config, solver->dims, solver->opts, solver->mem,
                                    solver->work, field, stage, index, sens_nlp_out);
    return;
}



void ocp_nlp_get(ocp_nlp_config *config, ocp_nlp_solver *solver,
                 const char *field, void *return_value_)
{
    solver->config->get(solver->config, solver->dims, solver->mem, field, return_value_);
}


void ocp_nlp_set(ocp_nlp_config *config, ocp_nlp_solver *solver,
        int stage, const char *field, void *value)
{
    ocp_nlp_memory *mem;
    config->get(config, solver->dims, solver->mem, "nlp_mem", &mem);
    // printf("called getter: nlp_mem %p\n", mem);

    ocp_nlp_dims *dims = solver->dims;

    if (!strcmp(field, "z_guess"))
    {
        int nz = dims->nz[stage];
        int nx = dims->nx[stage];
        double *double_values = value;
        blasfeo_pack_dvec(nz, double_values, mem->sim_guess + stage, nx);
        mem->set_sim_guess[stage] = true;
        // printf("set z_guess\n");
        // blasfeo_print_exp_dvec(nz, mem->sim_guess+stage, nx);
    }
    else if (!strcmp(field, "xdot_guess"))
    {
        int nx = dims->nx[stage];
        double *double_values = value;
        blasfeo_pack_dvec(nx, double_values, &mem->sim_guess[stage], 0);
        mem->set_sim_guess[stage] = true;
    }
    else if (!strcmp(field, "gnsf_phi_guess"))
    {
        int nout;
        config->dynamics[stage]->dims_get(config->dynamics[stage], dims->dynamics[stage],
                                            "gnsf_nout", &nout);
        double *double_values = value;
        blasfeo_pack_dvec(nout, double_values, &mem->sim_guess[stage], 0);
        mem->set_sim_guess[stage] = true;
    }
    else
    {
        printf("\nerror: ocp_nlp_mem_set: field %s not available\n", field);
        exit(1);
    }
    // printf("exit ocp_nlp_set\n");
}
