/* libfastpm: */
#include <mpi.h>
#include <string.h>
#include "libfastpm.h"
#include "msg.h"
#include "pmic.h"
#include "pm2lpt.h"

int 
fastpm_2lpt_init(FastPM2LPTSolver * solver, int nc, double boxsize, double alloc_factor, MPI_Comm comm)
{
    msg_init(comm);
    msg_set_loglevel(verbose);
    solver->p = malloc(sizeof(PMStore));
    solver->pm = malloc(sizeof(PM));
    pm_store_init(solver->p);

    pm_store_alloc_evenly(solver->p, pow(nc, 3), 
        PACK_POS | PACK_VEL | PACK_ID | PACK_ACC | PACK_DX1 | PACK_DX2 | PACK_Q,
        alloc_factor, comm);

    pm_init_simple(solver->pm, solver->p, nc, boxsize, comm);

    return 0;
}

void 
fastpm_fill_deltak(PM * pm, float_t * delta_k, int seed, fastpm_pkfunc pk, void * pkdata) 
{
    pmic_fill_gaussian_gadget(pm, delta_k, seed, pk, pkdata);
}

void 
fastpm_2lpt_evolve(FastPM2LPTSolver * solver, 
        float_t * delta_k_i, double aout, double omega_m)
{
    /* evolve particles by 2lpt to time a. pm->canvas contains rho(x, a) */
    double shift[3] = {0, 0, 0};

    pm_store_set_lagrangian_position(solver->p, solver->pm, shift);

    pm_2lpt_solve(solver->pm, delta_k_i, solver->p, shift);

    /* pdata->dx1 and pdata->dx2 are s1 and s2 terms 
     * S = D * dx1 + D2 * 3 / 7 * D20 * dx2; 
     *
     * See pmsteps.c 
     * */

    /* now shift particles to the correct locations. */

    /* predict particle positions by 2lpt */
    pm_2lpt_evolve(aout, solver->p, omega_m);
}

static void 
get_lagrangian_position(void * pdata, ptrdiff_t index, double pos[3]) 
{
    PMStore * p = (PMStore *)pdata;
    pos[0] = p->q[index][0];
    pos[1] = p->q[index][1];
    pos[2] = p->q[index][2];
}

void fastpm_apply_diff_transfer(PM * pm, float_t * from, float_t * to, int dir) {

    PMKFactors * fac[3];

    pm_create_k_factors(pm, fac);

#pragma omp parallel 
    {
        ptrdiff_t ind;
        ptrdiff_t start, end;
        ptrdiff_t i[3];

        pm_prepare_omp_loop(pm, &start, &end, i);

        for(ind = start; ind < end; ind += 2) {
            double k_finite = fac[dir][i[dir] + pm->ORegion.start[dir]].k;

            /* - i k[d] */
            to[ind + 0] =   from[ind + 1] * (k_finite);
            to[ind + 1] = - from[ind + 0] * (k_finite);
            pm_inc_o_index(pm, i);
        }
    }
    pm_destroy_k_factors(pm, fac);

}

void fastpm_apply_hmc_force_2lpt_transfer(PM * pm, float_t * from, float_t * to, int dir) {

    PMKFactors * fac[3];

    pm_create_k_factors(pm, fac);

#pragma omp parallel 
    {
        ptrdiff_t ind;
        ptrdiff_t start, end;
        ptrdiff_t i[3];

        pm_prepare_omp_loop(pm, &start, &end, i);

        for(ind = start; ind < end; ind += 2) {
            int d;
            double k_finite = fac[dir][i[dir] + pm->ORegion.start[dir]].k;
            double kk_finite = 0.;
            for(d = 0; d < 3; d++) {
                kk_finite += fac[d][i[d] + pm->ORegion.start[d]].kk;
            }

            if(kk_finite == 0)
            {
                to[ind + 0] = 0;
                to[ind + 1] = 0;
            }
            else
            {
                /* - i k[d] / k**2 */
                to[ind + 0] =   from[ind + 1] * (k_finite / kk_finite);
                to[ind + 1] = - from[ind + 0] * (k_finite / kk_finite);
            }
            pm_inc_o_index(pm, i);
        }
    }
    pm_destroy_k_factors(pm, fac);

}

void 
fastpm_2lpt_hmc_force(FastPM2LPTSolver * solver,
        float_t * data_x, /* rhop in x-space*/
        float_t * Fk     /* (out) hmc force in fourier space */
        )
{
    int d;

    float_t * workspace = pm_alloc(solver->pm);
    float_t * workspace2 = pm_alloc(solver->pm);

    PMGhostData * pgd = pm_ghosts_create(solver->pm, solver->p, PACK_POS, NULL);

    /* Note the 1.0 see the other comment on pm_paint in this file.*/
    pm_paint(solver->pm, workspace, solver->p, solver->p->np + pgd->nghosts, 1.0);

    ptrdiff_t ind;

    for(ind = 0; ind < solver->pm->allocsize; ind ++) {
        workspace[ind] -= data_x[ind];
    }

    pm_r2c(solver->pm, workspace, Fk);

    /* Fk contains rhod_k at this point */

    int ACC[] = {PACK_ACC_X, PACK_ACC_Y, PACK_ACC_Z};

    for(d = 0; d < 3; d ++) {
        fastpm_apply_diff_transfer(solver->pm, Fk, workspace, d);

        /* workspace stores \Gamma(k) = -i k \rho_d */

        pm_c2r(solver->pm, workspace);
        
        int i;
        /* acc stores \Gamma(x) := \Psi(q) */
        for(i = 0; i < solver->p->np + pgd->nghosts; i ++) {
            solver->p->acc[i][d] = pm_readout_one(solver->pm, workspace, solver->p, i) / solver->pm->Norm;
        }
        pm_ghosts_reduce(pgd, ACC[d]);
    }

    pm_ghosts_free(pgd);

    /* now we paint \Psi by the lagrangian position q */

    pgd = pm_ghosts_create(solver->pm, solver->p, PACK_Q, get_lagrangian_position);

    memset(workspace, 0, sizeof(workspace[0]) * solver->pm->allocsize);
    memset(workspace2, 0, sizeof(workspace2[0]) * solver->pm->allocsize);
    memset(Fk, 0, sizeof(Fk[0]) * solver->pm->allocsize);

    for(d = 0; d < 3; d ++) {
        int i;
        for(i = 0; i < solver->p->np + pgd->nghosts; i ++) {
            double pos[3];
            get_lagrangian_position(solver->p, i, pos);
            pm_paint_pos(solver->pm, workspace, pos, solver->p->acc[i][d]);
        }
        pm_r2c(solver->pm, workspace, workspace2);
        fastpm_apply_hmc_force_2lpt_transfer(solver->pm, workspace2, workspace, d);

        /* add HMC force component to to Fk */
        ptrdiff_t ind;
        for(ind = 0; ind < solver->pm->allocsize; ind ++) {
            Fk[ind] += 2 * workspace[ind] / solver->pm->Norm; 
            /*Wang's magic factor of 2 in 1301.1348. 
             * We do not put it in in hmc_force_2lpt_transfer */
        }
    }
    pm_ghosts_free(pgd);
    pm_free(solver->pm, workspace2);
    pm_free(solver->pm, workspace);
}

void
fastpm_2lpt_paint(FastPM2LPTSolver * solver, float_t * delta_x, float_t * delta_k) 
{

    PMGhostData * pgd = pm_ghosts_create(solver->pm, solver->p, PACK_POS, NULL);

    /* since for 2lpt we have on average 1 particle per cell, use 1.0 here.
     * otherwise increase this to (Nmesh / Ngrid) **3 */
    pm_paint(solver->pm, delta_x, solver->p, solver->p->np + pgd->nghosts, 1.0);

    pm_ghosts_free(pgd);

    if(delta_k) {
        pm_r2c(solver->pm, delta_x, delta_k);
    }
}

static double 
tk_eh(double k, struct fastpm_powerspec_eh_params * params)		/* from Martin White */
{
    double q, theta, ommh2, a, s, gamma, L0, C0;
    double tmp;
    double omegam, ombh2, hubble;

    /* other input parameters */
    hubble = params->hubble_param;
    omegam = params->omegam;
    ombh2 = params->omegab * hubble * hubble;

    theta = 2.728 / 2.7;
    ommh2 = omegam * hubble * hubble;
    s = 44.5 * log(9.83 / ommh2) / sqrt(1. + 10. * exp(0.75 * log(ombh2))) * hubble;
    a = 1. - 0.328 * log(431. * ommh2) * ombh2 / ommh2
        + 0.380 * log(22.3 * ommh2) * (ombh2 / ommh2) * (ombh2 / ommh2);
    gamma = a + (1. - a) / (1. + exp(4 * log(0.43 * k * s)));
    gamma *= omegam * hubble;
    q = k * theta * theta / gamma;
    L0 = log(2. * exp(1.) + 1.8 * q);
    C0 = 14.2 + 731. / (1. + 62.5 * q);
    tmp = L0 / (L0 + C0 * q * q);
    return (tmp);
}

double 
fastpm_powerspec_eh(double k, struct fastpm_powerspec_eh_params * param)	/* Eisenstein & Hu */
{
    return param->Norm * k * pow(tk_eh(k, param), 2);
}

