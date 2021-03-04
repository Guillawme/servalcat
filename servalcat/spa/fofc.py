# TODO shift map first using mask!
"""
Author: "Keitaro Yamashita, Garib N. Murshudov"
MRC Laboratory of Molecular Biology
    
This software is released under the
Mozilla Public License, version 2.0; see LICENSE.
"""
from __future__ import absolute_import, division, print_function, generators
import gemmi
import numpy
import time
from servalcat.utils import logger
from servalcat import utils
import argparse

def add_arguments(parser):
    parser.description = 'Fo-Fc map calculation based on model and data errors'
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--halfmaps", nargs=2)
    group.add_argument("--map", help="Use only if you really do not have half maps.")
    parser.add_argument('--model', required=True,
                        help='Input atomic model file (PDB or mmCIF/PDBx')
    parser.add_argument("-d", '--resolution', type=float, required=True)
    parser.add_argument('-m', '--mask', help="mask file")
    parser.add_argument('-r', '--mask_radius', type=float, help="mask radius")
    parser.add_argument("-B", type=float, help="Estimated blurring.")
    parser.add_argument("--normalized_map", action='store_true',
                        help="Write normalized map in the masked region")
    parser.add_argument("--crop", action='store_true',
                        help="Write cropped maps")
    parser.add_argument("--monlib",
                        help="Monomer library path. Default: $CLIBD_MON")
    parser.add_argument("--omit_proton", action='store_true',
                        help="Omit proton from model in map calculation")
    parser.add_argument("--omit_h_electron", action='store_true',
                        help="Omit hydrogen electrons from model in map calculation")
    parser.add_argument('--output_prefix', default="diffmap",
                        help='output file name prefix')
# add_arguments()

def parse_args(arg_list):
    parser = argparse.ArgumentParser()
    add_arguments(parser)
    return parser.parse_args(arg_list)
# parse_args()

def mask_and_fft_maps(maps, d_min, mask=None):
    assert len(maps) <= 2
    asus = []
    for m in maps:
        if mask is not None:
            m.grid = gemmi.FloatGrid(numpy.array(m.grid)*mask,
                                     m.grid.unit_cell, m.grid.spacegroup)

        asus.append(gemmi.transform_map_to_f_phi(m.grid).prepare_asu_data(dmin=d_min))

    if len(maps) == 2:
        df = utils.hkl.df_from_asu_data(asus[0], "F_map1")
        hkldata = utils.hkl.HklData(asus[0].unit_cell, asus[0].spacegroup, False, df)
        hkldata.merge_asu_data(asus[1], "F_map2")
        hkldata.df["FP"] = (hkldata.df.F_map1 + hkldata.df.F_map2)/2.
    else:
        df = utils.hkl.df_from_asu_data(asus[0], "FP")
        hkldata = utils.hkl.HklData(asus[0].unit_cell, asus[0].spacegroup, False, df)
        
    return hkldata
# mask_and_fft_maps()
    
def calc_noise_var(hkldata):
    # Scale
    #iniscale = utils.scaling.InitialScaler(fo_asu, fc_asu, aniso=True)
    #iniscale.run()
    #scale_for_fo = iniscale.get_scales()
    #fo_asu.value_array[:] *= scale_for_fo
    #asu1.value_array[:] *= scale_for_fo
    #asu2.value_array[:] *= scale_for_fo

    s_array = 1./hkldata.d_spacings()
    hkldata.binned_df["var_noise"] = 0.
    hkldata.binned_df["FSCfull"] = 0.
    
    logger.write("Bin Ncoeffs d_max   d_min   FSChalf var.noise   scale")
    F_map1 = numpy.array(hkldata.df.F_map1)
    F_map2 = numpy.array(hkldata.df.F_map2)
    for i_bin, bin_d_max, bin_d_min in hkldata.bin_and_limits():
        sel = i_bin == hkldata.df.bin
        # scale
        scale = 1. #numpy.sqrt(var_cmpl(fc)/var_cmpl(fo))
        #hkldata.df.loc[sel, "FP"] *= scale
        #hkldata.df.loc[sel, "F_map1"] *= scale
        #hkldata.df.loc[sel, "F_map2"] *= scale
        
        sel1 = F_map1[sel]
        sel2 = F_map2[sel]

        if sel1.size < 3:
            logger.write("WARNING: skipping bin {} with size= {}".format(i_bin, sel1.size))
            continue

        fsc = numpy.real(numpy.corrcoef(sel1, sel2)[1,0])
        varn = numpy.var(sel1-sel2)/4
        logger.write("{:3d} {:7d} {:7.3f} {:7.3f} {:.4f} {:e} {}".format(i_bin, sel1.size, bin_d_max, bin_d_min,
                                                                         fsc, varn, scale))
        hkldata.binned_df.loc[i_bin, "var_noise"] = varn
        hkldata.binned_df.loc[i_bin, "FSCfull"] = 2*fsc/(1+fsc)
# calc_noise_var()

def calc_D_and_S(hkldata, output_prefix, has_halfmaps=True):#fo_asu, fc_asu, varn, bins, bin_idxes):
    bdf = hkldata.binned_df
    bdf["D"] = 0.
    bdf["S"] = 0.
    ofs = open("{}_Fstats.dat".format(output_prefix), "w")
    ofs.write("bin       n   d_max   d_min         Fo         Fc FSC.model FSC.full      D          S          N\n")
    tmpl = "{:3d} {:7d} {:7.3f} {:7.3f} {:.4e} {:.4e} {: .4f}   {: .4f} {: .4e} {:.4e} {:.4e}\n"
    FP = numpy.array(hkldata.df.FP)
    FC = numpy.array(hkldata.df.FC)
    for i_bin, bin_d_max, bin_d_min in hkldata.bin_and_limits():
        sel = i_bin == hkldata.df.bin
        Fo = FP[sel]
        Fc = FC[sel]
        fsc = numpy.real(numpy.corrcoef(Fo, Fc)[1,0])
        bdf.loc[i_bin, "D"] = numpy.sum(numpy.real(Fo * numpy.conj(Fc)))/numpy.sum(numpy.abs(Fc)**2)
        if has_halfmaps:
            varn = hkldata.binned_df.var_noise[i_bin]
            fsc_full = hkldata.binned_df.FSCfull[i_bin]
            bdf.loc[i_bin, "S"] = max(0, numpy.average(numpy.abs(Fo-bdf.D[i_bin]*Fc)**2)-varn)
        else:
            varn = fsc_full = 0
        ofs.write(tmpl.format(i_bin, Fo.size, bin_d_max, bin_d_min,
                              numpy.average(numpy.abs(Fo)),
                              numpy.average(numpy.abs(Fc)),
                              fsc, fsc_full, bdf.D[i_bin], bdf.S[i_bin], varn))
# calc_D_and_S()

#import line_profiler
#profile = line_profiler.LineProfiler()
#import atexit
#atexit.register(profile.print_stats)
#@profile
def calc_maps(hkldata, B=None, has_halfmaps=True):
    if has_halfmaps:
        labs = ["DELFWT", "FWT", "DELFWT_noscale", "FWT_noscale"]
        if B is not None: labs.extend(["DELFWT_b0", "FWT_b0"])
    else:
        labs = ["DELFWT"]
        
    tmp = {}
    for l in labs:
        tmp[l] = numpy.zeros(len(hkldata.df.index), numpy.complex128)

    logger.write("Calculating maps..")
    time_t = time.time()

    FP = numpy.array(hkldata.df.FP)
    FC = numpy.array(hkldata.df.FC)
    
    for i_bin, bin_d_max, bin_d_min in hkldata.bin_and_limits():
        sel = i_bin == hkldata.df.bin
        Fo = FP[sel]
        Fc = FC[sel]
        D = hkldata.binned_df.D[i_bin]
        if not has_halfmaps:
            delfwt = (Fo-D*Fc)
            tmp["DELFWT"][sel] = delfwt
            continue

        S = hkldata.binned_df.S[i_bin]
        FSCfull = hkldata.binned_df.FSCfull[i_bin]
        varn = hkldata.binned_df.var_noise[i_bin]

        delfwt = (Fo-D*Fc)*S/(S+varn)
        fwt = (Fo*S+varn*D*Fc)/(S+varn)

        tmp["DELFWT_noscale"][sel] = delfwt
        tmp["FWT_noscale"][sel] = fwt

        sig_fo = numpy.std(Fo)
        n_fo = sig_fo * numpy.sqrt(FSCfull)
        if n_fo < 1e-10 or n_fo != n_fo:
            logger.write("WARNING: skipping bin {} sig_fo={} FSCfull={}".format(i_bin, sig_fo, FSCfull))
            continue
        #n_fofc = numpy.sqrt(var_cmpl(Fo-D*Fc))

        lab_suf = "" if B is None else "_b0"
        tmp["DELFWT"+lab_suf][sel] = delfwt/n_fo
        tmp["FWT"+lab_suf][sel] = fwt/n_fo

        if B is not None:
            s2 = 1./hkldata.d_spacings()[sel]**2
            k = numpy.exp(-B*s2/4.)
            k2 = numpy.exp(-B*s2/2.)
            fsc_l = k2*FSCfull/(1+(k2-1)*FSCfull)
            #n_fo = sig_fo * numpy.sqrt(fsc_l) * k
            S_l = S * k2
            
            delfwt = (Fo-D*Fc)*S*k/(S_l+varn)/n_fo # S_l/k = S*k
            fwt = (Fo*S_l+varn*D*Fc)/(S_l+varn)/n_fo/k
            logger.write("{:4d} {:.4e} {:.4e} {:.4e} {:.4e} {:.4e} {:.4e}".format(i_bin,
                                                                           numpy.average(n_fo),
                                                                           numpy.average(sig_fo),
                                                                           numpy.average(fsc_l),
                                                                           numpy.average(k),
                                                                           numpy.average(abs(fwt)),
                                                                           numpy.average(abs(delfwt))))
            tmp["DELFWT"][sel] = delfwt
            tmp["FWT"][sel] = fwt

    for l in labs:
        hkldata.df[l] = tmp[l]

    logger.write(" finished in {:.3f} sec.".format(time.time()-time_t))
    return labs
# calc_maps()

def dump_to_mtz(hkldata, map_labs, mtz_out):
    map_labs = map_labs + ["FP", "FC"]
    data = numpy.empty((len(hkldata.df.index), len(map_labs)*2+3))
    data[:,:3] = hkldata.df[["H","K","L"]]
    for i, lab in enumerate(map_labs):
        data[:,3+i*2] = numpy.abs(hkldata.df[lab])
        data[:,3+i*2+1] = numpy.angle(hkldata.df[lab], deg=True)
        
    mtz = gemmi.Mtz()
    mtz.spacegroup = hkldata.sg
    mtz.cell = hkldata.cell
    mtz.add_dataset('HKL_base')
    for label in ['H', 'K', 'L']: mtz.add_column(label, 'H')

    for lab in map_labs:
        mtz.add_column(lab, "F")
        mtz.add_column(("PH"+lab).replace("FWT", "WT"), "P")
    
    mtz.set_data(data)
    mtz.write_to_file(mtz_out)
# dump_to_mtz()
        
def main(args):
    if not args.halfmaps and not args.map:
        logger.write("Error: give --halfmaps or --map")
        return

    if not args.halfmaps and args.B is not None:
        logger.write("Error: -B only works for half maps")
        return

    if (args.omit_proton or args.omit_h_electron) and st[0].count_hydrogen_sites() == 0:
        logger.write("ERROR! --omit_proton/--omit_h_electron requested, but no hydrogen atoms were found.")
        return

    if not args.halfmaps:
        logger.write("Warning: using --halfmaps is strongly recommended!")

    st = gemmi.read_structure(args.model)
    st.expand_ncs(gemmi.HowToNameCopiedChain.Short)

    if args.halfmaps:
        maps = [utils.fileio.read_ccp4_map(f) for f in args.halfmaps]
        has_halfmaps = True
    else:
        maps = [utils.fileio.read_ccp4_map(args.map)]
        has_halfmaps = False

    g = maps[0].grid
    st.spacegroup_hm = "P1"
    st.cell = g.unit_cell

    if st[0].count_hydrogen_sites() > 0:
        monlib = utils.model.load_monomer_library(st[0].get_all_residue_names(),
                                                  monomer_dir=args.monlib)
    else:
        monlib = None

    if args.mask:
        mask = numpy.array(utils.fileio.read_ccp4_map(args.mask).grid)
    elif args.mask_radius:
        mask = gemmi.FloatGrid(*g.shape)
        mask.set_unit_cell(g.unit_cell)
        mask.spacegroup = gemmi.SpaceGroup(1)
        mask.mask_points_in_constant_radius(st[0], args.mask_radius, 1.)
        ccp4 = gemmi.Ccp4Map()
        ccp4.grid = mask
        ccp4.update_ccp4_header(2, True) # float, update stats
        ccp4.write_ccp4_map("mask_from_model.ccp4")
        mask = numpy.array(mask)
    else:
        mask = None
        if args.normalized_map:
            logger.write("WARNING: Mask is not available. --normalized-map will have no effect.")

    utils.model.normalize_it92(st)
    fc_asu = utils.model.calc_fc_fft(st, args.resolution, r_cut=1e-7, monlib=monlib, source="electron")

    hkldata = mask_and_fft_maps(maps, args.resolution, mask)
    hkldata.merge_asu_data(fc_asu, "FC")
    hkldata.setup_relion_binning()
    
    if args.halfmaps:
        calc_noise_var(hkldata)

    if args.B is not None:
        Bave = numpy.average(utils.model.all_B(st))
        logger.write("Using user-specified B: {}".format(args.B))
        logger.write("    Average B of model= {:.2f}".format(Bave))
        B = args.B - Bave
        logger.write("    Relative B for map= {:.2f}".format(B))
    else:
        B = None
        
    calc_D_and_S(hkldata, args.output_prefix, has_halfmaps=has_halfmaps)

    if args.omit_proton or args.omit_h_electron:
        fc_asu_2 = utils.model.calc_fc_fft(st, args.resolution, r_cut=1e-7, monlib=monlib, source="electron",
                                           omit_proton=args.omit_proton, omit_h_electron=args.omit_h_electron)
        del hkldata.df["FC"]
        hkldata.merge_asu_data(fc_asu_2, "FC")
    
    map_labs = calc_maps(hkldata, B=B, has_halfmaps=has_halfmaps)
    dump_to_mtz(hkldata, map_labs, "{}.mtz".format(args.output_prefix))

    if args.normalized_map and mask is not None:
        logger.write("Normalized Fo-Fc map requested.")
        delfwt_map = hkldata.fft_map("DELFWT", grid_size=mask.shape)
        cutoff = 1. - 1e-6
        masked = numpy.array(delfwt_map)[mask>cutoff]
        logger.write("   Whole volume: {} voxels".format(delfwt_map.point_count))
        logger.write("  Masked volume: {} voxels (>{})".format(masked.size, cutoff))
        global_mean = numpy.average(delfwt_map)
        global_std = numpy.std(delfwt_map)
        logger.write("    Global mean: {}".format(global_mean))
        logger.write("     Global std: {}".format(global_std))
        masked_mean = numpy.average(masked)
        masked_std = numpy.std(masked)
        logger.write("    Masked mean: {}".format(masked_mean))
        logger.write("     Masked std: {}".format(masked_std))
        #logger.write(" If you want to scale manually: {}".format())
        scaled = (delfwt_map - masked_mean)/masked_std
        filename = "{}_normalized_fofc.mrc".format(args.output_prefix)
        logger.write("  Writing {}".format(filename))
        utils.maps.write_ccp4_map(filename, scaled, cell=st.cell,
                                  mask_for_extent=mask if args.crop else None)

# main()

if __name__ == "__main__":
    import sys
    args = parse_args(sys.argv[1:])
    main(args)
