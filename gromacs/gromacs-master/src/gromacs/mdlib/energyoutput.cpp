/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2016,2017,2018,2019, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
#include "gmxpre.h"

#include "energyoutput.h"

#include <cfloat>
#include <cstdlib>
#include <cstring>

#include <string>

#include "gromacs/awh/awh.h"
#include "gromacs/fileio/enxio.h"
#include "gromacs/fileio/gmxfio.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/listed_forces/disre.h"
#include "gromacs/listed_forces/orires.h"
#include "gromacs/math/functions.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/constr.h"
#include "gromacs/mdlib/ebin.h"
#include "gromacs/mdlib/mdebin_bar.h"
#include "gromacs/mdtypes/energyhistory.h"
#include "gromacs/mdtypes/fcdata.h"
#include "gromacs/mdtypes/group.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/pulling/pull.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/trajectory/energyframe.h"
#include "gromacs/utility/arraysize.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"

static const char *conrmsd_nm[] = { "Constr. rmsd", "Constr.2 rmsd" };

static const char *boxs_nm[] = { "Box-X", "Box-Y", "Box-Z" };

static const char *tricl_boxs_nm[] = {
    "Box-XX", "Box-YY", "Box-ZZ",
    "Box-YX", "Box-ZX", "Box-ZY"
};

static const char *vol_nm[] = { "Volume" };

static const char *dens_nm[] = {"Density" };

static const char *pv_nm[] = {"pV" };

static const char *enthalpy_nm[] = {"Enthalpy" };

static const char *boxvel_nm[] = {
    "Box-Vel-XX", "Box-Vel-YY", "Box-Vel-ZZ",
    "Box-Vel-YX", "Box-Vel-ZX", "Box-Vel-ZY"
};

#define NBOXS asize(boxs_nm)
#define NTRICLBOXS asize(tricl_boxs_nm)

const char *egrp_nm[egNR+1] = {
    "Coul-SR", "LJ-SR", "Buck-SR",
    "Coul-14", "LJ-14", nullptr
};

/* forward declaration */
typedef struct t_mde_delta_h_coll t_mde_delta_h_coll;

namespace gmx
{

namespace detail
{

/* This is the collection of energy averages collected during mdrun, and to
   be written out to the .edr file. */
struct t_mdebin
{
    double              delta_t;
    t_ebin             *ebin;
    int                 ie, iconrmsd, ib, ivol, idens, ipv, ienthalpy;
    int                 isvir, ifvir, ipres, ivir, isurft, ipc, itemp, itc, itcb, iu, imu;
    int                 ivcos, ivisc;
    int                 nE, nEg, nEc, nTC, nTCP, nU, nNHC;
    int                *igrp;
    int                 mde_n, mdeb_n;
    real               *tmp_r;
    rvec               *tmp_v;
    gmx_bool            bConstr;
    gmx_bool            bConstrVir;
    gmx_bool            bTricl;
    gmx_bool            bDynBox;
    gmx_bool            bNHC_trotter;
    gmx_bool            bPrintNHChains;
    gmx_bool            bMTTK;
    gmx_bool            bMu; /* true if dipole is calculated */
    gmx_bool            bDiagPres;
    gmx_bool            bPres;
    int                 f_nre;
    int                 epc;
    real                ref_p;
    int                 etc;
    int                 nCrmsd;
    gmx_bool            bEner[F_NRE];
    gmx_bool            bEInd[egNR];

    FILE               *fp_dhdl; /* the dhdl.xvg output file */
    double             *dE;      /* energy components for dhdl.xvg output */
    t_mde_delta_h_coll *dhc;     /* the delta U components (raw data + histogram) */
    real               *temperatures;
};

}   // namespace detail

using detail::t_mdebin;

namespace
{

//! Legacy init function
t_mdebin *init_mdebin(ener_file_t       fp_ene,
                      const gmx_mtop_t *mtop,
                      const t_inputrec *ir,
                      const pull_t     *pull_work,
                      FILE             *fp_dhdl,
                      bool              isRerun)
{
    const char                *ener_nm[F_NRE];
    static const char         *vir_nm[] = {
        "Vir-XX", "Vir-XY", "Vir-XZ",
        "Vir-YX", "Vir-YY", "Vir-YZ",
        "Vir-ZX", "Vir-ZY", "Vir-ZZ"
    };
    static const char         *sv_nm[] = {
        "ShakeVir-XX", "ShakeVir-XY", "ShakeVir-XZ",
        "ShakeVir-YX", "ShakeVir-YY", "ShakeVir-YZ",
        "ShakeVir-ZX", "ShakeVir-ZY", "ShakeVir-ZZ"
    };
    static const char         *fv_nm[] = {
        "ForceVir-XX", "ForceVir-XY", "ForceVir-XZ",
        "ForceVir-YX", "ForceVir-YY", "ForceVir-YZ",
        "ForceVir-ZX", "ForceVir-ZY", "ForceVir-ZZ"
    };
    static const char         *pres_nm[] = {
        "Pres-XX", "Pres-XY", "Pres-XZ",
        "Pres-YX", "Pres-YY", "Pres-YZ",
        "Pres-ZX", "Pres-ZY", "Pres-ZZ"
    };
    static const char         *surft_nm[] = {
        "#Surf*SurfTen"
    };
    static const char         *mu_nm[] = {
        "Mu-X", "Mu-Y", "Mu-Z"
    };
    static const char         *vcos_nm[] = {
        "2CosZ*Vel-X"
    };
    static const char         *visc_nm[] = {
        "1/Viscosity"
    };
    static const char         *baro_nm[] = {
        "Barostat"
    };

    const SimulationGroups    *groups;
    char                     **gnm;
    char                       buf[256];
    const char                *bufi;
    t_mdebin                  *md;
    int                        i, j, ni, nj, n, k, kk, ncon, nset;
    gmx_bool                   bBHAM, b14;

    snew(md, 1);

    if (EI_DYNAMICS(ir->eI))
    {
        md->delta_t = ir->delta_t;
    }
    else
    {
        md->delta_t = 0;
    }

    groups = &mtop->groups;

    bBHAM = (mtop->ffparams.numTypes() > 0) && (mtop->ffparams.functype[0] == F_BHAM);
    b14   = (gmx_mtop_ftype_count(mtop, F_LJ14) > 0 ||
             gmx_mtop_ftype_count(mtop, F_LJC14_Q) > 0);

    ncon           = gmx_mtop_ftype_count(mtop, F_CONSTR);
    nset           = gmx_mtop_ftype_count(mtop, F_SETTLE);
    md->bConstr    = (ncon > 0 || nset > 0) && !isRerun;
    md->bConstrVir = FALSE;
    if (md->bConstr)
    {
        if (ncon > 0 && ir->eConstrAlg == econtLINCS)
        {
            md->nCrmsd = 1;
        }
        md->bConstrVir = (getenv("GMX_CONSTRAINTVIR") != nullptr);
    }
    else
    {
        md->nCrmsd = 0;
    }

    /* Energy monitoring */
    for (i = 0; i < egNR; i++)
    {
        md->bEInd[i] = FALSE;
    }

    for (i = 0; i < F_NRE; i++)
    {
        md->bEner[i] = FALSE;
        if (isRerun &&
            (i == F_EKIN || i == F_ETOT || i == F_ECONSERVED ||
             i == F_TEMP || i == F_PDISPCORR || i == F_PRES))
        {
            continue;
        }
        if (i == F_LJ)
        {
            md->bEner[i] = !bBHAM;
        }
        else if (i == F_BHAM)
        {
            md->bEner[i] = bBHAM;
        }
        else if (i == F_EQM)
        {
            md->bEner[i] = ir->bQMMM;
        }
        else if (i == F_RF_EXCL)
        {
            md->bEner[i] = (EEL_RF(ir->coulombtype) && ir->cutoff_scheme == ecutsGROUP);
        }
        else if (i == F_COUL_RECIP)
        {
            md->bEner[i] = EEL_FULL(ir->coulombtype);
        }
        else if (i == F_LJ_RECIP)
        {
            md->bEner[i] = EVDW_PME(ir->vdwtype);
        }
        else if (i == F_LJ14)
        {
            md->bEner[i] = b14;
        }
        else if (i == F_COUL14)
        {
            md->bEner[i] = b14;
        }
        else if (i == F_LJC14_Q || i == F_LJC_PAIRS_NB)
        {
            md->bEner[i] = FALSE;
        }
        else if ((i == F_DVDL_COUL && ir->fepvals->separate_dvdl[efptCOUL]) ||
                 (i == F_DVDL_VDW  && ir->fepvals->separate_dvdl[efptVDW]) ||
                 (i == F_DVDL_BONDED && ir->fepvals->separate_dvdl[efptBONDED]) ||
                 (i == F_DVDL_RESTRAINT && ir->fepvals->separate_dvdl[efptRESTRAINT]) ||
                 (i == F_DKDL && ir->fepvals->separate_dvdl[efptMASS]) ||
                 (i == F_DVDL && ir->fepvals->separate_dvdl[efptFEP]))
        {
            md->bEner[i] = (ir->efep != efepNO);
        }
        else if ((interaction_function[i].flags & IF_VSITE) ||
                 (i == F_CONSTR) || (i == F_CONSTRNC) || (i == F_SETTLE))
        {
            md->bEner[i] = FALSE;
        }
        else if ((i == F_COUL_SR) || (i == F_EPOT) || (i == F_PRES)  || (i == F_EQM))
        {
            md->bEner[i] = TRUE;
        }
        else if ((i == F_ETOT) || (i == F_EKIN) || (i == F_TEMP))
        {
            md->bEner[i] = EI_DYNAMICS(ir->eI);
        }
        else if (i == F_DISPCORR || i == F_PDISPCORR)
        {
            md->bEner[i] = (ir->eDispCorr != edispcNO);
        }
        else if (i == F_DISRESVIOL)
        {
            md->bEner[i] = (gmx_mtop_ftype_count(mtop, F_DISRES) > 0);
        }
        else if (i == F_ORIRESDEV)
        {
            md->bEner[i] = (gmx_mtop_ftype_count(mtop, F_ORIRES) > 0);
        }
        else if (i == F_CONNBONDS)
        {
            md->bEner[i] = FALSE;
        }
        else if (i == F_COM_PULL)
        {
            md->bEner[i] = ((ir->bPull && pull_have_potential(pull_work)) ||
                            ir->bRot || ir->bColvars);
        }
        else if (i == F_ECONSERVED)
        {
            md->bEner[i] = (integratorHasConservedEnergyQuantity(ir));
        }
        else
        {
            md->bEner[i] = (gmx_mtop_ftype_count(mtop, i) > 0);
        }
    }

    md->f_nre = 0;
    for (i = 0; i < F_NRE; i++)
    {
        if (md->bEner[i])
        {
            ener_nm[md->f_nre] = interaction_function[i].longname;
            md->f_nre++;
        }
    }

    md->epc            = isRerun ? epcNO : ir->epc;
    md->bDiagPres      = !TRICLINIC(ir->ref_p) && !isRerun;
    md->ref_p          = (ir->ref_p[XX][XX]+ir->ref_p[YY][YY]+ir->ref_p[ZZ][ZZ])/DIM;
    md->bTricl         = TRICLINIC(ir->compress) || TRICLINIC(ir->deform);
    md->bDynBox        = inputrecDynamicBox(ir);
    md->etc            = isRerun ? etcNO : ir->etc;
    md->bNHC_trotter   = inputrecNvtTrotter(ir) && !isRerun;
    md->bPrintNHChains = ir->bPrintNHChains && !isRerun;
    md->bMTTK          = (inputrecNptTrotter(ir) || inputrecNphTrotter(ir)) && !isRerun;
    md->bMu            = inputrecNeedMutot(ir);
    md->bPres          = !isRerun;

    md->ebin  = mk_ebin();
    /* Pass NULL for unit to let get_ebin_space determine the units
     * for interaction_function[i].longname
     */
    md->ie    = get_ebin_space(md->ebin, md->f_nre, ener_nm, nullptr);
    if (md->nCrmsd)
    {
        /* This should be called directly after the call for md->ie,
         * such that md->iconrmsd follows directly in the list.
         */
        md->iconrmsd = get_ebin_space(md->ebin, md->nCrmsd, conrmsd_nm, "");
    }
    if (md->bDynBox)
    {
        md->ib    = get_ebin_space(md->ebin,
                                   md->bTricl ? NTRICLBOXS : NBOXS,
                                   md->bTricl ? tricl_boxs_nm : boxs_nm,
                                   unit_length);
        md->ivol  = get_ebin_space(md->ebin, 1, vol_nm,  unit_volume);
        md->idens = get_ebin_space(md->ebin, 1, dens_nm, unit_density_SI);
        if (md->bDiagPres)
        {
            md->ipv       = get_ebin_space(md->ebin, 1, pv_nm,   unit_energy);
            md->ienthalpy = get_ebin_space(md->ebin, 1, enthalpy_nm,   unit_energy);
        }
    }
    if (md->bConstrVir)
    {
        md->isvir = get_ebin_space(md->ebin, asize(sv_nm), sv_nm, unit_energy);
        md->ifvir = get_ebin_space(md->ebin, asize(fv_nm), fv_nm, unit_energy);
    }
    if (md->bPres)
    {
        md->ivir   = get_ebin_space(md->ebin, asize(vir_nm), vir_nm, unit_energy);
        md->ipres  = get_ebin_space(md->ebin, asize(pres_nm), pres_nm, unit_pres_bar);
        md->isurft = get_ebin_space(md->ebin, asize(surft_nm), surft_nm,
                                    unit_surft_bar);
    }
    if (md->epc == epcPARRINELLORAHMAN || md->epc == epcMTTK)
    {
        md->ipc = get_ebin_space(md->ebin, md->bTricl ? 6 : 3,
                                 boxvel_nm, unit_vel);
    }
    if (md->bMu)
    {
        md->imu    = get_ebin_space(md->ebin, asize(mu_nm), mu_nm, unit_dipole_D);
    }
    if (ir->cos_accel != 0)
    {
        md->ivcos = get_ebin_space(md->ebin, asize(vcos_nm), vcos_nm, unit_vel);
        md->ivisc = get_ebin_space(md->ebin, asize(visc_nm), visc_nm,
                                   unit_invvisc_SI);
    }

    /* Energy monitoring */
    for (i = 0; i < egNR; i++)
    {
        md->bEInd[i] = FALSE;
    }
    md->bEInd[egCOULSR] = TRUE;
    md->bEInd[egLJSR  ] = TRUE;

    if (bBHAM)
    {
        md->bEInd[egLJSR]   = FALSE;
        md->bEInd[egBHAMSR] = TRUE;
    }
    if (b14)
    {
        md->bEInd[egLJ14]   = TRUE;
        md->bEInd[egCOUL14] = TRUE;
    }
    md->nEc = 0;
    for (i = 0; (i < egNR); i++)
    {
        if (md->bEInd[i])
        {
            md->nEc++;
        }
    }
    n       = groups->groups[SimulationAtomGroupType::EnergyOutput].size();
    md->nEg = n;
    md->nE  = (n*(n+1))/2;

    snew(md->igrp, md->nE);
    if (md->nE > 1)
    {
        n = 0;
        snew(gnm, md->nEc);
        for (k = 0; (k < md->nEc); k++)
        {
            snew(gnm[k], STRLEN);
        }
        for (i = 0; (i < gmx::ssize(groups->groups[SimulationAtomGroupType::EnergyOutput])); i++)
        {
            ni = groups->groups[SimulationAtomGroupType::EnergyOutput][i];
            for (j = i; (j < gmx::ssize(groups->groups[SimulationAtomGroupType::EnergyOutput])); j++)
            {
                nj = groups->groups[SimulationAtomGroupType::EnergyOutput][j];
                for (k = kk = 0; (k < egNR); k++)
                {
                    if (md->bEInd[k])
                    {
                        sprintf(gnm[kk], "%s:%s-%s", egrp_nm[k],
                                *(groups->groupNames[ni]), *(groups->groupNames[nj]));
                        kk++;
                    }
                }
                md->igrp[n] = get_ebin_space(md->ebin, md->nEc,
                                             gnm, unit_energy);
                n++;
            }
        }
        for (k = 0; (k < md->nEc); k++)
        {
            sfree(gnm[k]);
        }
        sfree(gnm);

        if (n != md->nE)
        {
            gmx_incons("Number of energy terms wrong");
        }
    }

    md->nTC  = isRerun ? 0 : groups->groups[SimulationAtomGroupType::TemperatureCoupling].size();
    md->nNHC = ir->opts.nhchainlength; /* shorthand for number of NH chains */
    if (md->bMTTK)
    {
        md->nTCP = 1;  /* assume only one possible coupling system for barostat
                          for now */
    }
    else
    {
        md->nTCP = 0;
    }
    if (md->etc == etcNOSEHOOVER)
    {
        if (md->bNHC_trotter)
        {
            md->mde_n = 2*md->nNHC*md->nTC;
        }
        else
        {
            md->mde_n = 2*md->nTC;
        }
        if (md->epc == epcMTTK)
        {
            md->mdeb_n = 2*md->nNHC*md->nTCP;
        }
    }
    else
    {
        md->mde_n  = md->nTC;
        md->mdeb_n = 0;
    }

    snew(md->tmp_r, md->mde_n);

    // TODO redo the group name memory management to make it more clear
    char **grpnms;
    snew(grpnms, std::max(md->mde_n, md->mdeb_n)); // Just in case md->mdeb_n > md->mde_n

    for (i = 0; (i < md->nTC); i++)
    {
        ni = groups->groups[SimulationAtomGroupType::TemperatureCoupling][i];
        sprintf(buf, "T-%s", *(groups->groupNames[ni]));
        grpnms[i] = gmx_strdup(buf);
    }
    md->itemp = get_ebin_space(md->ebin, md->nTC, grpnms,
                               unit_temp_K);
    for (i = 0; i < md->nTC; i++)
    {
        sfree(grpnms[i]);
    }

    int allocated = 0;
    if (md->etc == etcNOSEHOOVER)
    {
        if (md->bPrintNHChains)
        {
            if (md->bNHC_trotter)
            {
                for (i = 0; (i < md->nTC); i++)
                {
                    ni   = groups->groups[SimulationAtomGroupType::TemperatureCoupling][i];
                    bufi = *(groups->groupNames[ni]);
                    for (j = 0; (j < md->nNHC); j++)
                    {
                        sprintf(buf, "Xi-%d-%s", j, bufi);
                        grpnms[2*(i*md->nNHC+j)] = gmx_strdup(buf);
                        sprintf(buf, "vXi-%d-%s", j, bufi);
                        grpnms[2*(i*md->nNHC+j)+1] = gmx_strdup(buf);
                    }
                }
                md->itc = get_ebin_space(md->ebin, md->mde_n,
                                         grpnms, unit_invtime);
                allocated = md->mde_n;
                if (md->bMTTK)
                {
                    for (i = 0; (i < md->nTCP); i++)
                    {
                        bufi = baro_nm[0];  /* All barostat DOF's together for now. */
                        for (j = 0; (j < md->nNHC); j++)
                        {
                            sprintf(buf, "Xi-%d-%s", j, bufi);
                            grpnms[2*(i*md->nNHC+j)] = gmx_strdup(buf);
                            sprintf(buf, "vXi-%d-%s", j, bufi);
                            grpnms[2*(i*md->nNHC+j)+1] = gmx_strdup(buf);
                        }
                    }
                    md->itcb = get_ebin_space(md->ebin, md->mdeb_n,
                                              grpnms, unit_invtime);
                    allocated = md->mdeb_n;
                }
            }
            else
            {
                for (i = 0; (i < md->nTC); i++)
                {
                    ni   = groups->groups[SimulationAtomGroupType::TemperatureCoupling][i];
                    bufi = *(groups->groupNames[ni]);
                    sprintf(buf, "Xi-%s", bufi);
                    grpnms[2*i] = gmx_strdup(buf);
                    sprintf(buf, "vXi-%s", bufi);
                    grpnms[2*i+1] = gmx_strdup(buf);
                }
                md->itc = get_ebin_space(md->ebin, md->mde_n,
                                         grpnms, unit_invtime);
                allocated = md->mde_n;
            }
        }
    }
    else if (md->etc == etcBERENDSEN || md->etc == etcYES ||
             md->etc == etcVRESCALE)
    {
        for (i = 0; (i < md->nTC); i++)
        {
            ni = groups->groups[SimulationAtomGroupType::TemperatureCoupling][i];
            sprintf(buf, "Lamb-%s", *(groups->groupNames[ni]));
            grpnms[i] = gmx_strdup(buf);
        }
        md->itc   = get_ebin_space(md->ebin, md->mde_n, grpnms, "");
        allocated = md->mde_n;
    }

    for (i = 0; i < allocated; i++)
    {
        sfree(grpnms[i]);
    }
    sfree(grpnms);

    md->nU = groups->groups[SimulationAtomGroupType::Acceleration].size();
    snew(md->tmp_v, md->nU);
    if (md->nU > 1)
    {
        snew(grpnms, 3*md->nU);
        for (i = 0; (i < md->nU); i++)
        {
            ni = groups->groups[SimulationAtomGroupType::Acceleration][i];
            sprintf(buf, "Ux-%s", *(groups->groupNames[ni]));
            grpnms[3*i+XX] = gmx_strdup(buf);
            sprintf(buf, "Uy-%s", *(groups->groupNames[ni]));
            grpnms[3*i+YY] = gmx_strdup(buf);
            sprintf(buf, "Uz-%s", *(groups->groupNames[ni]));
            grpnms[3*i+ZZ] = gmx_strdup(buf);
        }
        md->iu = get_ebin_space(md->ebin, 3*md->nU, grpnms, unit_vel);
        for (i = 0; i < 3*md->nU; i++)
        {
            sfree(grpnms[i]);
        }
        sfree(grpnms);
    }

    if (fp_ene)
    {
        do_enxnms(fp_ene, &md->ebin->nener, &md->ebin->enm);
    }

    /* check whether we're going to write dh histograms */
    md->dhc = nullptr;
    if (ir->fepvals->separate_dhdl_file == esepdhdlfileNO)
    {
        /* Currently dh histograms are only written with dynamics */
        if (EI_DYNAMICS(ir->eI))
        {
            snew(md->dhc, 1);

            mde_delta_h_coll_init(md->dhc, ir);
        }
        md->fp_dhdl = nullptr;
        snew(md->dE, ir->fepvals->n_lambda);
    }
    else
    {
        md->fp_dhdl = fp_dhdl;
        snew(md->dE, ir->fepvals->n_lambda);
    }
    if (ir->bSimTemp)
    {
        int i;
        snew(md->temperatures, ir->fepvals->n_lambda);
        for (i = 0; i < ir->fepvals->n_lambda; i++)
        {
            md->temperatures[i] = ir->simtempvals->temperatures[i];
        }
    }
    return md;
}

//! Legacy cleanup function
void done_mdebin(t_mdebin *mdebin)
{
    sfree(mdebin->igrp);
    sfree(mdebin->tmp_r);
    sfree(mdebin->tmp_v);
    done_ebin(mdebin->ebin);
    done_mde_delta_h_coll(mdebin->dhc);
    sfree(mdebin->dE);
    sfree(mdebin->temperatures);
    sfree(mdebin);
}

} // namespace
} // namespace gmx

/* print a lambda vector to a string
   fep = the inputrec's FEP input data
   i = the index of the lambda vector
   get_native_lambda = whether to print the native lambda
   get_names = whether to print the names rather than the values
   str = the pre-allocated string buffer to print to. */
static void print_lambda_vector(t_lambda *fep, int i,
                                gmx_bool get_native_lambda, gmx_bool get_names,
                                char *str)
{
    int    j, k = 0;
    int    Nsep = 0;

    for (j = 0; j < efptNR; j++)
    {
        if (fep->separate_dvdl[j])
        {
            Nsep++;
        }
    }
    str[0] = 0; /* reset the string */
    if (Nsep > 1)
    {
        str += sprintf(str, "("); /* set the opening parenthesis*/
    }
    for (j = 0; j < efptNR; j++)
    {
        if (fep->separate_dvdl[j])
        {
            if (!get_names)
            {
                if (get_native_lambda && fep->init_lambda >= 0)
                {
                    str += sprintf(str, "%.4f", fep->init_lambda);
                }
                else
                {
                    str += sprintf(str, "%.4f", fep->all_lambda[j][i]);
                }
            }
            else
            {
                str += sprintf(str, "%s", efpt_singular_names[j]);
            }
            /* print comma for the next item */
            if (k < Nsep-1)
            {
                str += sprintf(str, ", ");
            }
            k++;
        }
    }
    if (Nsep > 1)
    {
        /* and add the closing parenthesis */
        sprintf(str, ")");
    }
}

FILE *open_dhdl(const char *filename, const t_inputrec *ir,
                const gmx_output_env_t *oenv)
{
    FILE       *fp;
    const char *dhdl = "dH/d\\lambda", *deltag = "\\DeltaH", *lambda = "\\lambda",
    *lambdastate     = "\\lambda state";
    int         i, nsets, nsets_de, nsetsbegin;
    int         n_lambda_terms = 0;
    t_lambda   *fep            = ir->fepvals; /* for simplicity */
    t_expanded *expand         = ir->expandedvals;
    char        lambda_vec_str[STRLEN], lambda_name_str[STRLEN];

    int         nsets_dhdl = 0;
    int         s          = 0;
    int         nsetsextend;
    gmx_bool    write_pV = FALSE;

    /* count the number of different lambda terms */
    for (i = 0; i < efptNR; i++)
    {
        if (fep->separate_dvdl[i])
        {
            n_lambda_terms++;
        }
    }

    std::string title, label_x, label_y;
    if (fep->n_lambda == 0)
    {
        title   = gmx::formatString("%s", dhdl);
        label_x = gmx::formatString("Time (ps)");
        label_y = gmx::formatString("%s (%s %s)",
                                    dhdl, unit_energy, "[\\lambda]\\S-1\\N");
    }
    else
    {
        title   = gmx::formatString("%s and %s", dhdl, deltag);
        label_x = gmx::formatString("Time (ps)");
        label_y = gmx::formatString("%s and %s (%s %s)",
                                    dhdl, deltag, unit_energy, "[\\8l\\4]\\S-1\\N");
    }
    fp = gmx_fio_fopen(filename, "w+");
    xvgr_header(fp, title.c_str(), label_x, label_y, exvggtXNY, oenv);

    std::string buf;
    if (!(ir->bSimTemp))
    {
        buf = gmx::formatString("T = %g (K) ", ir->opts.ref_t[0]);
    }
    if ((ir->efep != efepSLOWGROWTH) && (ir->efep != efepEXPANDED))
    {
        if ( (fep->init_lambda >= 0)  && (n_lambda_terms == 1 ))
        {
            /* compatibility output */
            buf += gmx::formatString("%s = %.4f", lambda, fep->init_lambda);
        }
        else
        {
            print_lambda_vector(fep, fep->init_fep_state, TRUE, FALSE,
                                lambda_vec_str);
            print_lambda_vector(fep, fep->init_fep_state, TRUE, TRUE,
                                lambda_name_str);
            buf += gmx::formatString("%s %d: %s = %s",
                                     lambdastate, fep->init_fep_state,
                                     lambda_name_str, lambda_vec_str);
        }
    }
    xvgr_subtitle(fp, buf.c_str(), oenv);


    nsets_dhdl = 0;
    if (fep->dhdl_derivatives == edhdlderivativesYES)
    {
        nsets_dhdl = n_lambda_terms;
    }
    /* count the number of delta_g states */
    nsets_de = fep->lambda_stop_n - fep->lambda_start_n;

    nsets = nsets_dhdl + nsets_de; /* dhdl + fep differences */

    if (fep->n_lambda > 0 && (expand->elmcmove > elmcmoveNO))
    {
        nsets += 1;   /*add fep state for expanded ensemble */
    }

    if (fep->edHdLPrintEnergy != edHdLPrintEnergyNO)
    {
        nsets += 1;  /* add energy to the dhdl as well */
    }

    nsetsextend = nsets;
    if ((ir->epc != epcNO) && (fep->n_lambda > 0) && (fep->init_lambda < 0))
    {
        nsetsextend += 1; /* for PV term, other terms possible if required for
                             the reduced potential (only needed with foreign
                             lambda, and only output when init_lambda is not
                             set in order to maintain compatibility of the
                             dhdl.xvg file) */
        write_pV     = TRUE;
    }
    std::vector<std::string> setname(nsetsextend);

    if (expand->elmcmove > elmcmoveNO)
    {
        /* state for the fep_vals, if we have alchemical sampling */
        setname[s++] = "Thermodynamic state";
    }

    if (fep->edHdLPrintEnergy != edHdLPrintEnergyNO)
    {
        std::string energy;
        switch (fep->edHdLPrintEnergy)
        {
            case edHdLPrintEnergyPOTENTIAL:
                energy = gmx::formatString("%s (%s)", "Potential Energy", unit_energy);
                break;
            case edHdLPrintEnergyTOTAL:
            case edHdLPrintEnergyYES:
            default:
                energy = gmx::formatString("%s (%s)", "Total Energy", unit_energy);
        }
        setname[s++] = energy;
    }

    if (fep->dhdl_derivatives == edhdlderivativesYES)
    {
        for (i = 0; i < efptNR; i++)
        {
            if (fep->separate_dvdl[i])
            {
                std::string derivative;
                if ( (fep->init_lambda >= 0)  && (n_lambda_terms == 1 ))
                {
                    /* compatibility output */
                    derivative = gmx::formatString("%s %s %.4f", dhdl, lambda, fep->init_lambda);
                }
                else
                {
                    double lam = fep->init_lambda;
                    if (fep->init_lambda < 0)
                    {
                        lam = fep->all_lambda[i][fep->init_fep_state];
                    }
                    derivative = gmx::formatString("%s %s = %.4f", dhdl, efpt_singular_names[i],
                                                   lam);
                }
                setname[s++] = derivative;
            }
        }
    }

    if (fep->n_lambda > 0)
    {
        /* g_bar has to determine the lambda values used in this simulation
         * from this xvg legend.
         */

        if (expand->elmcmove > elmcmoveNO)
        {
            nsetsbegin = 1;  /* for including the expanded ensemble */
        }
        else
        {
            nsetsbegin = 0;
        }

        if (fep->edHdLPrintEnergy != edHdLPrintEnergyNO)
        {
            nsetsbegin += 1;
        }
        nsetsbegin += nsets_dhdl;

        for (i = fep->lambda_start_n; i < fep->lambda_stop_n; i++)
        {
            print_lambda_vector(fep, i, FALSE, FALSE, lambda_vec_str);
            std::string buf;
            if ( (fep->init_lambda >= 0)  && (n_lambda_terms == 1 ))
            {
                /* for compatible dhdl.xvg files */
                buf = gmx::formatString("%s %s %s", deltag, lambda, lambda_vec_str);
            }
            else
            {
                buf = gmx::formatString("%s %s to %s", deltag, lambda, lambda_vec_str);
            }

            if (ir->bSimTemp)
            {
                /* print the temperature for this state if doing simulated annealing */
                buf += gmx::formatString("T = %g (%s)",
                                         ir->simtempvals->temperatures[s-(nsetsbegin)],
                                         unit_temp_K);
            }
            setname[s++] = buf;
        }
        if (write_pV)
        {
            setname[s++] = gmx::formatString("pV (%s)", unit_energy);
        }

        xvgrLegend(fp, setname, oenv);
    }

    return fp;
}

namespace gmx
{
namespace
{

//! Legacy update function
void upd_mdebin(t_mdebin               *md,
                gmx_bool                bDoDHDL,
                gmx_bool                bSum,
                double                  time,
                real                    tmass,
                gmx_enerdata_t         *enerd,
                t_state                *state,
                t_lambda               *fep,
                t_expanded             *expand,
                matrix                  box,
                tensor                  svir,
                tensor                  fvir,
                tensor                  vir,
                tensor                  pres,
                gmx_ekindata_t         *ekind,
                rvec                    mu_tot,
                const gmx::Constraints *constr)
{
    int    j, k, kk, n, gid;
    real   crmsd[2], tmp6[6];
    real   bs[NTRICLBOXS], vol, dens, pv, enthalpy;
    real   eee[egNR];
    double store_dhdl[efptNR];
    real   store_energy = 0;
    real   tmp;

    /* Do NOT use the box in the state variable, but the separate box provided
     * as an argument. This is because we sometimes need to write the box from
     * the last timestep to match the trajectory frames.
     */
    add_ebin_indexed(md->ebin, md->ie, gmx::ArrayRef<gmx_bool>(md->bEner), enerd->term, bSum);
    if (md->nCrmsd)
    {
        crmsd[0] = constr->rmsd();
        add_ebin(md->ebin, md->iconrmsd, md->nCrmsd, crmsd, FALSE);
    }
    if (md->bDynBox)
    {
        int nboxs;
        if (md->bTricl)
        {
            bs[0] = box[XX][XX];
            bs[1] = box[YY][YY];
            bs[2] = box[ZZ][ZZ];
            bs[3] = box[YY][XX];
            bs[4] = box[ZZ][XX];
            bs[5] = box[ZZ][YY];
            nboxs = NTRICLBOXS;
        }
        else
        {
            bs[0] = box[XX][XX];
            bs[1] = box[YY][YY];
            bs[2] = box[ZZ][ZZ];
            nboxs = NBOXS;
        }
        vol  = box[XX][XX]*box[YY][YY]*box[ZZ][ZZ];
        dens = (tmass*AMU)/(vol*NANO*NANO*NANO);
        add_ebin(md->ebin, md->ib, nboxs, bs, bSum);
        add_ebin(md->ebin, md->ivol, 1, &vol, bSum);
        add_ebin(md->ebin, md->idens, 1, &dens, bSum);

        if (md->bDiagPres)
        {
            /* This is pV (in kJ/mol).  The pressure is the reference pressure,
               not the instantaneous pressure */
            pv = vol*md->ref_p/PRESFAC;

            add_ebin(md->ebin, md->ipv, 1, &pv, bSum);
            enthalpy = pv + enerd->term[F_ETOT];
            add_ebin(md->ebin, md->ienthalpy, 1, &enthalpy, bSum);
        }
    }
    if (md->bConstrVir)
    {
        add_ebin(md->ebin, md->isvir, 9, svir[0], bSum);
        add_ebin(md->ebin, md->ifvir, 9, fvir[0], bSum);
    }
    if (md->bPres)
    {
        add_ebin(md->ebin, md->ivir, 9, vir[0], bSum);
        add_ebin(md->ebin, md->ipres, 9, pres[0], bSum);
        tmp = (pres[ZZ][ZZ]-(pres[XX][XX]+pres[YY][YY])*0.5)*box[ZZ][ZZ];
        add_ebin(md->ebin, md->isurft, 1, &tmp, bSum);
    }
    if (md->epc == epcPARRINELLORAHMAN || md->epc == epcMTTK)
    {
        tmp6[0] = state->boxv[XX][XX];
        tmp6[1] = state->boxv[YY][YY];
        tmp6[2] = state->boxv[ZZ][ZZ];
        tmp6[3] = state->boxv[YY][XX];
        tmp6[4] = state->boxv[ZZ][XX];
        tmp6[5] = state->boxv[ZZ][YY];
        add_ebin(md->ebin, md->ipc, md->bTricl ? 6 : 3, tmp6, bSum);
    }
    if (md->bMu)
    {
        add_ebin(md->ebin, md->imu, 3, mu_tot, bSum);
    }
    if (ekind && ekind->cosacc.cos_accel != 0)
    {
        vol  = box[XX][XX]*box[YY][YY]*box[ZZ][ZZ];
        dens = (tmass*AMU)/(vol*NANO*NANO*NANO);
        add_ebin(md->ebin, md->ivcos, 1, &(ekind->cosacc.vcos), bSum);
        /* 1/viscosity, unit 1/(kg m^-1 s^-1) */
        tmp = 1/(ekind->cosacc.cos_accel/(ekind->cosacc.vcos*PICO)
                 *dens*gmx::square(box[ZZ][ZZ]*NANO/(2*M_PI)));
        add_ebin(md->ebin, md->ivisc, 1, &tmp, bSum);
    }
    if (md->nE > 1)
    {
        n = 0;
        for (int i = 0; (i < md->nEg); i++)
        {
            for (j = i; (j < md->nEg); j++)
            {
                gid = GID(i, j, md->nEg);
                for (k = kk = 0; (k < egNR); k++)
                {
                    if (md->bEInd[k])
                    {
                        eee[kk++] = enerd->grpp.ener[k][gid];
                    }
                }
                add_ebin(md->ebin, md->igrp[n], md->nEc, eee, bSum);
                n++;
            }
        }
    }

    if (ekind)
    {
        for (int i = 0; (i < md->nTC); i++)
        {
            md->tmp_r[i] = ekind->tcstat[i].T;
        }
        add_ebin(md->ebin, md->itemp, md->nTC, md->tmp_r, bSum);

        if (md->etc == etcNOSEHOOVER)
        {
            /* whether to print Nose-Hoover chains: */
            if (md->bPrintNHChains)
            {
                if (md->bNHC_trotter)
                {
                    for (int i = 0; (i < md->nTC); i++)
                    {
                        for (j = 0; j < md->nNHC; j++)
                        {
                            k                = i*md->nNHC+j;
                            md->tmp_r[2*k]   = state->nosehoover_xi[k];
                            md->tmp_r[2*k+1] = state->nosehoover_vxi[k];
                        }
                    }
                    add_ebin(md->ebin, md->itc, md->mde_n, md->tmp_r, bSum);

                    if (md->bMTTK)
                    {
                        for (int i = 0; (i < md->nTCP); i++)
                        {
                            for (j = 0; j < md->nNHC; j++)
                            {
                                k                = i*md->nNHC+j;
                                md->tmp_r[2*k]   = state->nhpres_xi[k];
                                md->tmp_r[2*k+1] = state->nhpres_vxi[k];
                            }
                        }
                        add_ebin(md->ebin, md->itcb, md->mdeb_n, md->tmp_r, bSum);
                    }
                }
                else
                {
                    for (int i = 0; (i < md->nTC); i++)
                    {
                        md->tmp_r[2*i]   = state->nosehoover_xi[i];
                        md->tmp_r[2*i+1] = state->nosehoover_vxi[i];
                    }
                    add_ebin(md->ebin, md->itc, md->mde_n, md->tmp_r, bSum);
                }
            }
        }
        else if (md->etc == etcBERENDSEN || md->etc == etcYES ||
                 md->etc == etcVRESCALE)
        {
            for (int i = 0; (i < md->nTC); i++)
            {
                md->tmp_r[i] = ekind->tcstat[i].lambda;
            }
            add_ebin(md->ebin, md->itc, md->nTC, md->tmp_r, bSum);
        }
    }

    if (ekind && md->nU > 1)
    {
        for (int i = 0; (i < md->nU); i++)
        {
            copy_rvec(ekind->grpstat[i].u, md->tmp_v[i]);
        }
        add_ebin(md->ebin, md->iu, 3*md->nU, md->tmp_v[0], bSum);
    }

    ebin_increase_count(md->ebin, bSum);

    /* BAR + thermodynamic integration values */
    if ((md->fp_dhdl || md->dhc) && bDoDHDL)
    {
        for (gmx::index i = 0; i < static_cast<gmx::index>(enerd->enerpart_lambda.size()) - 1; i++)
        {
            /* zero for simulated tempering */
            md->dE[i] = enerd->enerpart_lambda[i+1]-enerd->enerpart_lambda[0];
            if (md->temperatures != nullptr)
            {
                /* MRS: is this right, given the way we have defined the exchange probabilities? */
                /* is this even useful to have at all? */
                md->dE[i] += (md->temperatures[i]/
                              md->temperatures[state->fep_state]-1.0)*
                    enerd->term[F_EKIN];
            }
        }

        if (md->fp_dhdl)
        {
            fprintf(md->fp_dhdl, "%.4f", time);
            /* the current free energy state */

            /* print the current state if we are doing expanded ensemble */
            if (expand->elmcmove > elmcmoveNO)
            {
                fprintf(md->fp_dhdl, " %4d", state->fep_state);
            }
            /* total energy (for if the temperature changes */

            if (fep->edHdLPrintEnergy != edHdLPrintEnergyNO)
            {
                switch (fep->edHdLPrintEnergy)
                {
                    case edHdLPrintEnergyPOTENTIAL:
                        store_energy = enerd->term[F_EPOT];
                        break;
                    case edHdLPrintEnergyTOTAL:
                    case edHdLPrintEnergyYES:
                    default:
                        store_energy = enerd->term[F_ETOT];
                }
                fprintf(md->fp_dhdl, " %#.8g", store_energy);
            }

            if (fep->dhdl_derivatives == edhdlderivativesYES)
            {
                for (int i = 0; i < efptNR; i++)
                {
                    if (fep->separate_dvdl[i])
                    {
                        /* assumes F_DVDL is first */
                        fprintf(md->fp_dhdl, " %#.8g", enerd->term[F_DVDL+i]);
                    }
                }
            }
            for (int i = fep->lambda_start_n; i < fep->lambda_stop_n; i++)
            {
                fprintf(md->fp_dhdl, " %#.8g", md->dE[i]);
            }
            if (md->bDynBox &&
                md->bDiagPres &&
                (md->epc != epcNO) &&
                !enerd->enerpart_lambda.empty() &&
                (fep->init_lambda < 0))
            {
                fprintf(md->fp_dhdl, " %#.8g", pv);  /* PV term only needed when
                                                        there are alternate state
                                                        lambda and we're not in
                                                        compatibility mode */
            }
            fprintf(md->fp_dhdl, "\n");
            /* and the binary free energy output */
        }
        if (md->dhc && bDoDHDL)
        {
            int idhdl = 0;
            for (int i = 0; i < efptNR; i++)
            {
                if (fep->separate_dvdl[i])
                {
                    /* assumes F_DVDL is first */
                    store_dhdl[idhdl] = enerd->term[F_DVDL+i];
                    idhdl            += 1;
                }
            }
            store_energy = enerd->term[F_ETOT];
            /* store_dh is dE */
            mde_delta_h_coll_add_dh(md->dhc,
                                    static_cast<double>(state->fep_state),
                                    store_energy,
                                    pv,
                                    store_dhdl,
                                    md->dE + fep->lambda_start_n,
                                    time);
        }
    }
}

}   // namespace

void EnergyOutput::recordNonEnergyStep()
{
    ebin_increase_count(mdebin->ebin, false);
}

namespace
{

//! Legacy output function
void npr(FILE *log, int n, char c)
{
    for (; (n > 0); n--)
    {
        fprintf(log, "%c", c);
    }
}

//! Legacy output function
void pprint(FILE *log, const char *s, t_mdebin *md)
{
    char CHAR = '#';
    int  slen;
    char buf1[22], buf2[22];

    slen = strlen(s);
    fprintf(log, "\t<======  ");
    npr(log, slen, CHAR);
    fprintf(log, "  ==>\n");
    fprintf(log, "\t<====  %s  ====>\n", s);
    fprintf(log, "\t<==  ");
    npr(log, slen, CHAR);
    fprintf(log, "  ======>\n\n");

    fprintf(log, "\tStatistics over %s steps using %s frames\n",
            gmx_step_str(md->ebin->nsteps_sim, buf1),
            gmx_step_str(md->ebin->nsum_sim, buf2));
    fprintf(log, "\n");
}

}   // namespace

void print_ebin_header(FILE *log, int64_t steps, double time)
{
    char buf[22];

    fprintf(log, "   %12s   %12s\n"
            "   %12s   %12.5f\n\n",
            "Step", "Time", gmx_step_str(steps, buf), time);
}

namespace
{

/*! \brief Print current values of thermodynamic parameters
 *
 * This function only does something useful when bEne || bDR || bOR || log.
 *
 * \param[in] fp_ene   Energy file for the output.
 * \param[in] bEne     If it is a step for energy output or last step.
 * \param[in] bDR      If it is a step of writing distance restraints.
 * \param[in] bOR      If it is a step of writing orientation restraints.
 * \param[in] log      Pointer to the log file.
 * \param[in] step     Current step.
 * \param[in] time     Current simulation time.
 * \param[in] md       Accumulated data for the system.
 * \param[in] fcd      Bonded force computation data,
 *                     including orientation and distance restraints.
 * \param[in] awh      AWH data.
 */
void printCurrentValues(ener_file_t fp_ene, gmx_bool bEne, gmx_bool bDR, gmx_bool bOR,
                        FILE *log,
                        int64_t step, double time,
                        t_mdebin *md, t_fcdata *fcd,
                        gmx::Awh *awh)
{
    t_enxframe   fr;
    init_enxframe(&fr);
    fr.t            = time;
    fr.step         = step;
    fr.nsteps       = md->ebin->nsteps;
    fr.dt           = md->delta_t;
    fr.nsum         = md->ebin->nsum;
    fr.nre          = (bEne) ? md->ebin->nener : 0;
    fr.ener         = md->ebin->e;
    int ndisre          = bDR ? fcd->disres.npair : 0;
    /* these are for the old-style blocks (1 subblock, only reals), because
       there can be only one per ID for these */
    int          nr[enxNR];
    int          id[enxNR];
    real        *block[enxNR];
    /* Optional additional old-style (real-only) blocks. */
    for (int i = 0; i < enxNR; i++)
    {
        nr[i] = 0;
    }

    if (bOR && fcd->orires.nr > 0)
    {
        diagonalize_orires_tensors(&(fcd->orires));
        nr[enxOR]     = fcd->orires.nr;
        block[enxOR]  = fcd->orires.otav;
        id[enxOR]     = enxOR;
        nr[enxORI]    = (fcd->orires.oinsl != fcd->orires.otav) ?
            fcd->orires.nr : 0;
        block[enxORI] = fcd->orires.oinsl;
        id[enxORI]    = enxORI;
        nr[enxORT]    = fcd->orires.nex*12;
        block[enxORT] = fcd->orires.eig;
        id[enxORT]    = enxORT;
    }

    /* whether we are going to write anything out: */
    if (fr.nre || ndisre || nr[enxOR] || nr[enxORI])
    {
        /* the old-style blocks go first */
        fr.nblock = 0;
        for (int i = 0; i < enxNR; i++)
        {
            if (nr[i] > 0)
            {
                fr.nblock = i + 1;
            }
        }
        add_blocks_enxframe(&fr, fr.nblock);
        for (int b = 0; b < fr.nblock; b++)
        {
            add_subblocks_enxblock(&(fr.block[b]), 1);
            fr.block[b].id        = id[b];
            fr.block[b].sub[0].nr = nr[b];
#if !GMX_DOUBLE
            fr.block[b].sub[0].type = xdr_datatype_float;
            fr.block[b].sub[0].fval = block[b];
#else
            fr.block[b].sub[0].type = xdr_datatype_double;
            fr.block[b].sub[0].dval = block[b];
#endif
        }

        /* check for disre block & fill it. */
        if (ndisre > 0)
        {
            int db = fr.nblock;
            fr.nblock += 1;
            add_blocks_enxframe(&fr, fr.nblock);

            add_subblocks_enxblock(&(fr.block[db]), 2);
            fr.block[db].id        = enxDISRE;
            fr.block[db].sub[0].nr = ndisre;
            fr.block[db].sub[1].nr = ndisre;
#if !GMX_DOUBLE
            fr.block[db].sub[0].type = xdr_datatype_float;
            fr.block[db].sub[1].type = xdr_datatype_float;
            fr.block[db].sub[0].fval = fcd->disres.rt;
            fr.block[db].sub[1].fval = fcd->disres.rm3tav;
#else
            fr.block[db].sub[0].type = xdr_datatype_double;
            fr.block[db].sub[1].type = xdr_datatype_double;
            fr.block[db].sub[0].dval = fcd->disres.rt;
            fr.block[db].sub[1].dval = fcd->disres.rm3tav;
#endif
        }
        /* here we can put new-style blocks */

        /* Free energy perturbation blocks */
        if (md->dhc)
        {
            mde_delta_h_coll_handle_block(md->dhc, &fr, fr.nblock);
        }

        /* we can now free & reset the data in the blocks */
        if (md->dhc)
        {
            mde_delta_h_coll_reset(md->dhc);
        }

        /* AWH bias blocks. */
        if (awh != nullptr)  // TODO: add boolean in t_mdebin. See in mdebin.h.
        {
            awh->writeToEnergyFrame(step, &fr);
        }

        /* do the actual I/O */
        do_enx(fp_ene, &fr);
        if (fr.nre)
        {
            /* We have stored the sums, so reset the sum history */
            reset_ebin_sums(md->ebin);
        }
    }
    free_enxframe(&fr);
    if (log)
    {
        if (bOR && fcd->orires.nr > 0)
        {
            print_orires_log(log, &(fcd->orires));
        }

        fprintf(log, "   Energies (%s)\n", unit_energy);
        pr_ebin(log, md->ebin, md->ie, md->f_nre+md->nCrmsd, 5, eprNORMAL, TRUE);
        fprintf(log, "\n");
    }
}

/*! \brief Print reference temperatures for annealing groups.
 *
 * This does something only when log is not nullptr.
 *
 * \param[in] log     Log file to print to.
 * \param[in] groups  Information on atom groups.
 * \param[in] opts    Atom temperature coupling groups options
 *                    (annealing is done by groups).
 *
 */
void printAnnealingReferenceTemperatures(FILE *log, SimulationGroups *groups, t_grpopts *opts)
{
    if (log)
    {
        if (opts)
        {
            for (int i = 0; i < opts->ngtc; i++)
            {
                if (opts->annealing[i] != eannNO)
                {
                    fprintf(log, "Current ref_t for group %s: %8.1f\n",
                            *(groups->groupNames[groups->groups[SimulationAtomGroupType::TemperatureCoupling][i]]),
                            opts->ref_t[i]);
                }
            }
            fprintf(log, "\n");
        }
    }
}

/*! \brief Prints average values
 *
 * This is called at the end of the simulation run to print accumulated average values.
 *
 * \param[in]   log      Where to print.
 * \param[in]   md       Accumulated data for the system.
 * \param[in]   groups   Atom groups.
 */
void printAverageValues(FILE             *log,
                        t_mdebin         *md,
                        SimulationGroups *groups)
{
    if (md->ebin->nsum_sim <= 0)
    {
        if (log)
        {
            fprintf(log, "Not enough data recorded to report energy averages\n");
        }
        return;
    }
    if (log)
    {
        pprint(log, "A V E R A G E S", md);

        fprintf(log, "   Energies (%s)\n", unit_energy);
        pr_ebin(log, md->ebin, md->ie, md->f_nre+md->nCrmsd, 5, eprAVER, TRUE);
        fprintf(log, "\n");

        if (md->bDynBox)
        {
            pr_ebin(log, md->ebin, md->ib, md->bTricl ? NTRICLBOXS : NBOXS, 5,
                    eprAVER, TRUE);
            fprintf(log, "\n");
        }
        if (md->bConstrVir)
        {
            fprintf(log, "   Constraint Virial (%s)\n", unit_energy);
            pr_ebin(log, md->ebin, md->isvir, 9, 3, eprAVER, FALSE);
            fprintf(log, "\n");
            fprintf(log, "   Force Virial (%s)\n", unit_energy);
            pr_ebin(log, md->ebin, md->ifvir, 9, 3, eprAVER, FALSE);
            fprintf(log, "\n");
        }
        if (md->bPres)
        {
            fprintf(log, "   Total Virial (%s)\n", unit_energy);
            pr_ebin(log, md->ebin, md->ivir, 9, 3, eprAVER, FALSE);
            fprintf(log, "\n");
            fprintf(log, "   Pressure (%s)\n", unit_pres_bar);
            pr_ebin(log, md->ebin, md->ipres, 9, 3, eprAVER, FALSE);
            fprintf(log, "\n");
        }
        if (md->bMu)
        {
            fprintf(log, "   Total Dipole (%s)\n", unit_dipole_D);
            pr_ebin(log, md->ebin, md->imu, 3, 3, eprAVER, FALSE);
            fprintf(log, "\n");
        }

        if (md->nE > 1)
        {
            int padding = 8 - strlen(unit_energy);
            fprintf(log, "%*sEpot (%s)   ", padding, "", unit_energy);
            for (int i = 0; (i < egNR); i++)
            {
                if (md->bEInd[i])
                {
                    fprintf(log, "%12s   ", egrp_nm[i]);
                }
            }
            fprintf(log, "\n");

            int n = 0;
            for (int i = 0; (i < md->nEg); i++)
            {
                int ni = groups->groups[SimulationAtomGroupType::EnergyOutput][i];
                for (int j = i; (j < md->nEg); j++)
                {
                    int nj      = groups->groups[SimulationAtomGroupType::EnergyOutput][j];
                    int padding = 14 - (strlen(*(groups->groupNames[ni])) +
                                        strlen(*(groups->groupNames[nj])));
                    fprintf(log, "%*s%s-%s", padding, "",
                            *(groups->groupNames[ni]),
                            *(groups->groupNames[nj]));
                    pr_ebin(log, md->ebin, md->igrp[n], md->nEc, md->nEc, eprAVER,
                            FALSE);
                    n++;
                }
            }
            fprintf(log, "\n");
        }
        if (md->nTC > 1)
        {
            pr_ebin(log, md->ebin, md->itemp, md->nTC, 4, eprAVER, TRUE);
            fprintf(log, "\n");
        }
        if (md->nU > 1)
        {
            fprintf(log, "%15s   %12s   %12s   %12s\n",
                    "Group", "Ux", "Uy", "Uz");
            for (int i = 0; (i < md->nU); i++)
            {
                int ni = groups->groups[SimulationAtomGroupType::Acceleration][i];
                fprintf(log, "%15s", *groups->groupNames[ni]);
                pr_ebin(log, md->ebin, md->iu+3*i, 3, 3, eprAVER, FALSE);
            }
            fprintf(log, "\n");
        }
    }
}

/*! \brief Legacy ebin output function.
 *
 * The function dispatches the printing according to what is to be printed
 *
 * TODO It is too many responsibilities for this function to handle
 *      both .edr and .log output for both per-time and time-average data.
 * TODO The split between average and current outputs is to be done into two separate
 *      methods of the EnergyOutput class. This function will then be removed.
 *
 * \param[in] fp_ene   Energy file for the output.
 * \param[in] bEne     If it is a step for energy output or last step.
 * \param[in] bDR      If it is a step of writing distance restraints.
 * \param[in] bOR      If it is a step of writing orientation restraints.
 * \param[in] log      Pointer to the log file.
 * \param[in] step     Current step.
 * \param[in] time     Current simulation time.
 * \param[in] mode     eprNORMAL to output current values,
 *                     eprAVER to output collected averages.
 * \param[in] md       Accumulated data for the system.
 * \param[in] fcd      Bonded force computation data,
 *                     including orientation and distance restraints.
 * \param[in] groups   Information on atom groups.
 * \param[in] opts     Atom groups.
 * \param[in] awh      AWH data.
 */
void print_ebin(ener_file_t fp_ene, gmx_bool bEne, gmx_bool bDR, gmx_bool bOR,
                FILE *log,
                int64_t step, double time,
                int mode,
                t_mdebin *md, t_fcdata *fcd,
                SimulationGroups *groups, t_grpopts *opts,
                gmx::Awh *awh)
{
    printAnnealingReferenceTemperatures(log, groups, opts);

    if (mode == eprNORMAL)
    {
        printCurrentValues(fp_ene, bEne, bDR, bOR,
                           log,
                           step, time,
                           md,   fcd,
                           awh);
    }

    if (mode == eprAVER)
    {
        printAverageValues(log,
                           md,
                           groups);
    }

}

//! Legacy update function
void update_energyhistory(energyhistory_t * enerhist, const t_mdebin * mdebin)
{
    const t_ebin * const ebin = mdebin->ebin;

    enerhist->nsteps     = ebin->nsteps;
    enerhist->nsum       = ebin->nsum;
    enerhist->nsteps_sim = ebin->nsteps_sim;
    enerhist->nsum_sim   = ebin->nsum_sim;

    if (ebin->nsum > 0)
    {
        /* This will only actually resize the first time */
        enerhist->ener_ave.resize(ebin->nener);
        enerhist->ener_sum.resize(ebin->nener);

        for (int i = 0; i < ebin->nener; i++)
        {
            enerhist->ener_ave[i] = ebin->e[i].eav;
            enerhist->ener_sum[i] = ebin->e[i].esum;
        }
    }

    if (ebin->nsum_sim > 0)
    {
        /* This will only actually resize the first time */
        enerhist->ener_sum_sim.resize(ebin->nener);

        for (int i = 0; i < ebin->nener; i++)
        {
            enerhist->ener_sum_sim[i] = ebin->e_sim[i].esum;
        }
    }
    if (mdebin->dhc)
    {
        mde_delta_h_coll_update_energyhistory(mdebin->dhc, enerhist);
    }
}

//! Legacy restore function
void restore_energyhistory_from_state(t_mdebin              * mdebin,
                                      const energyhistory_t * enerhist)
{
    unsigned int nener = static_cast<unsigned int>(mdebin->ebin->nener);

    GMX_RELEASE_ASSERT(enerhist, "Need valid history to restore");

    if ((enerhist->nsum     > 0 && nener != enerhist->ener_sum.size()) ||
        (enerhist->nsum_sim > 0 && nener != enerhist->ener_sum_sim.size()))
    {
        gmx_fatal(FARGS, "Mismatch between number of energies in run input (%u) and checkpoint file (%zu or %zu).",
                  nener, enerhist->ener_sum.size(), enerhist->ener_sum_sim.size());
    }

    mdebin->ebin->nsteps     = enerhist->nsteps;
    mdebin->ebin->nsum       = enerhist->nsum;
    mdebin->ebin->nsteps_sim = enerhist->nsteps_sim;
    mdebin->ebin->nsum_sim   = enerhist->nsum_sim;

    for (int i = 0; i < mdebin->ebin->nener; i++)
    {
        mdebin->ebin->e[i].eav  =
            (enerhist->nsum > 0 ? enerhist->ener_ave[i] : 0);
        mdebin->ebin->e[i].esum =
            (enerhist->nsum > 0 ? enerhist->ener_sum[i] : 0);
        mdebin->ebin->e_sim[i].esum =
            (enerhist->nsum_sim > 0 ? enerhist->ener_sum_sim[i] : 0);
    }
    if (mdebin->dhc)
    {
        mde_delta_h_coll_restore_energyhistory(mdebin->dhc, enerhist->deltaHForeignLambdas.get());
    }
}

}   // namespace

EnergyOutput::EnergyOutput()
    : mdebin(nullptr)
{
}

void EnergyOutput::prepare(ener_file        *fp_ene,
                           const gmx_mtop_t *mtop,
                           const t_inputrec *ir,
                           const pull_t     *pull_work,
                           FILE             *fp_dhdl,
                           bool              isRerun)
{
    mdebin = init_mdebin(fp_ene, mtop, ir, pull_work, fp_dhdl, isRerun);
}

EnergyOutput::~EnergyOutput()
{
    done_mdebin(mdebin);
}

t_ebin *EnergyOutput::getEbin()
{
    return mdebin->ebin;
}

void EnergyOutput::addDataAtEnergyStep(bool                    bDoDHDL,
                                       bool                    bSum,
                                       double                  time,
                                       real                    tmass,
                                       gmx_enerdata_t         *enerd,
                                       t_state                *state,
                                       t_lambda               *fep,
                                       t_expanded             *expand,
                                       matrix                  box,
                                       tensor                  svir,
                                       tensor                  fvir,
                                       tensor                  vir,
                                       tensor                  pres,
                                       gmx_ekindata_t         *ekind,
                                       rvec                    mu_tot,
                                       const gmx::Constraints *constr)
{
    upd_mdebin(mdebin, bDoDHDL, bSum, time, tmass, enerd, state, fep,
               expand, box, svir, fvir, vir, pres, ekind, mu_tot, constr);
}

void EnergyOutput::printStepToEnergyFile(ener_file *fp_ene, bool bEne, bool bDR, bool bOR,
                                         FILE *log,
                                         int64_t step, double time,
                                         int mode,
                                         t_fcdata *fcd,
                                         SimulationGroups *groups, t_grpopts *opts,
                                         gmx::Awh *awh)
{
    print_ebin(fp_ene, bEne, bDR, bOR, log, step, time, mode,
               mdebin, fcd, groups, opts, awh);
}

int EnergyOutput::numEnergyTerms() const
{
    return mdebin->ebin->nener;
}

void EnergyOutput::fillEnergyHistory(energyhistory_t *enerhist) const
{
    update_energyhistory(enerhist, mdebin);
}

void EnergyOutput::restoreFromEnergyHistory(const energyhistory_t &enerhist)
{
    restore_energyhistory_from_state(mdebin, &enerhist);
}

} // namespace gmx