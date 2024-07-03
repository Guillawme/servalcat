// Author: "Keitaro Yamashita, Garib N. Murshudov"
// MRC Laboratory of Molecular Biology

#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/complex.h>
#include <pybind11/eigen.h>
#include <gemmi/symmetry.hpp>
#include <vector>
#include <map>
#include <iostream>
#include "math.hpp"
namespace py = pybind11;
using namespace servalcat;

struct TwinData {
  std::vector<gemmi::Op::Miller> asu;
  std::vector<int> centric;
  std::vector<double> epsilon;
  std::vector<double> alphas;
  Eigen::MatrixXd fc_array;
  std::vector<int> bin;

  // References
  // this may be slow. should we use 1d array and access via function?
  std::vector<std::vector<size_t>> rb2o; // [i_block][i_obs] -> index of iobs
  std::vector<std::vector<size_t>> rb2a; // [i_block][i] -> index of asu (for P(F; Fc))
  std::vector<std::vector<std::vector<size_t>>> rbo2a; // [i_block][i_obs][i] -> index of rb2a
  std::vector<std::vector<std::vector<size_t>>> rbo2c; // [i_block][i_obs][i] -> index of alpha
  std::vector<int> rbin; // [i_block] -> bin

  void clear() {
    asu.clear();
    alphas.clear();
    rb2o.clear();
    rb2a.clear();
    rbo2a.clear();
    rbo2c.clear();
  }

  int idx_of_asu(const gemmi::Op::Miller &h) const {
    auto it = std::lower_bound(asu.begin(), asu.end(), h);
    if (it != asu.end() && *it == h)
      return std::distance(asu.begin(), it);
    throw std::runtime_error("hkl not found in asu");
  }
  size_t n_obs() const {
    size_t ret = 0;
    for (const auto x : rb2o)
      ret += x.size();
    return ret;
  }
  size_t n_ops() const { // include identity
    return rbo2a.front().front().size();
  }

  void setup(const std::vector<gemmi::Op::Miller> &hkls,
             const std::vector<int> &bins,
             const gemmi::SpaceGroup &sg,
             const std::vector<gemmi::Op> &operators) {
    clear();
    const gemmi::GroupOps gops = sg.operations();
    const gemmi::ReciprocalAsu rasu(&sg);
    auto apply_and_asu = [&rasu, &gops](const gemmi::Op &op, const gemmi::Op::Miller &h) {
      return rasu.to_asu(op.apply_to_hkl(h), gops).first;
    };
    alphas.assign(operators.size() + 1, 0.);
    std::map<gemmi::Op::Miller, int> bin_map;
    // Set asu
    for (int i = 0; i < hkls.size(); ++i) {
      const auto h = hkls[i];
      // assuming hkl is in ASU - but may not be complete?
      asu.push_back(h);
      bin_map.emplace(h, bins[i]);
      for (const auto &op : operators) {
        const auto hr = apply_and_asu(op, h);
        asu.push_back(hr);
        bin_map.emplace(hr, bins[i]); // this isn't always correct, if pseudo-merohedral
      }
    }
    std::sort(asu.begin(), asu.end());
    asu.erase(std::unique(asu.begin(), asu.end()), asu.end());
    epsilon.reserve(asu.size());
    centric.reserve(asu.size());
    bin.reserve(asu.size());
    for (const auto &h : asu) {
      epsilon.push_back(gops.epsilon_factor_without_centering(h));
      centric.push_back(gops.is_reflection_centric(h));
      bin.push_back(bin_map[h]);
    }
    // Permutation sort
    std::vector<int> perm(hkls.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::sort(perm.begin(), perm.end(),
              [&](int lhs, int rhs) {return hkls[lhs] < hkls[rhs];});
    // Loop over hkls
    std::vector<bool> done(hkls.size());
    for (int i = 0; i < perm.size(); ++i) {
      const auto &h = hkls[perm[i]];
      if (done[perm[i]]) continue;
      rbin.push_back(bins[perm[i]]); // first encounter
      rbo2a.emplace_back();
      rbo2c.emplace_back();
      rb2o.emplace_back();
      rb2a.emplace_back(1, idx_of_asu(h));
      // loop over same hkls (would not happen if unique set was given)
      for (int j = i; j < perm.size() && hkls[perm[j]] == h; ++j) {
        rb2o.back().push_back(perm[j]);
        done[perm[j]] = true;
      }
      // loop over twin related
      for (const auto &op : operators) {
        const auto hr = apply_and_asu(op, h);
        for (auto it = std::lower_bound(perm.begin(), perm.end(), hr,
                                        [&](int lhs, const gemmi::Op::Miller &rhs) {return hkls[lhs] < rhs;});
             it != perm.end() && hkls[*it] == hr; ++it) {
          size_t j = std::distance(perm.begin(), it);
          if (done[perm[j]]) continue;
          rb2o.back().push_back(perm[j]);
          done[perm[j]] = true;
        }
        rb2a.back().push_back(idx_of_asu(hr));
      }
      std::sort(rb2a.back().begin(), rb2a.back().end());
      rb2a.back().erase(std::unique(rb2a.back().begin(), rb2a.back().end()), rb2a.back().end());
      const auto idx_of_rb2a = [&](size_t h) {
        auto it = std::lower_bound(rb2a.back().begin(), rb2a.back().end(), h);
        if (it != rb2a.back().end() && *it == h)
          return std::distance(rb2a.back().begin(), it);
        throw std::runtime_error("hkl not found in rb2a"); // should not happen
      };
      for (auto j : rb2o.back()) {
        const auto &h2 = hkls[j];
        rbo2a.back().emplace_back(1, idx_of_rb2a(idx_of_asu(h2)));
        rbo2c.back().emplace_back(1, 0);
        for (int k = 0; k < operators.size(); ++k) {
          const auto h2r = apply_and_asu(operators[k], h2);
          rbo2a.back().back().push_back(idx_of_rb2a(idx_of_asu(h2r)));
          rbo2c.back().back().push_back(k + 1);
        }
      }
    }
  }

  // calculation of f(x), which is part of -LL = \int exp(-f(x)) dx
  double calc_f(int ib, double *iobs, double *sigo, std::complex<double> *f_calc, double S, const Eigen::VectorXd &f_true) const {
    double ret = 0;
    for (int io = 0; io < rb2o[ib].size(); ++io) {
      const int obs_idx = rb2o[ib][io];
      if (std::isnan(iobs[obs_idx]))
        continue;
      double i_true_twin = 0;
      for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic)
        i_true_twin += alphas[rbo2c[ib][io][ic]] * gemmi::sq(f_true(rbo2a[ib][io][ic]));
      ret += gemmi::sq((iobs[obs_idx] - i_true_twin) / sigo[obs_idx]) * 0.5;
    }
    for (int ia = 0; ia < rb2a[ib].size(); ++ia) {
      const int a_idx = rb2a[ib][ia];
      const int c = centric[a_idx];
      const double den = epsilon[a_idx] * S;
      ret += (gemmi::sq(f_true(ia)) + std::norm(f_calc[a_idx])) / den / (1. + c);
      const double X = std::abs(f_calc[a_idx]) * f_true(ia) / den;
      ret -= log_i0_or_cosh(X, c + 1);
      if (c == 0) // acentric
        ret -= std::log(f_true(ia));
    }
    return ret;
  }

  // first and second derivative matrix of f(x)
  std::pair<Eigen::VectorXd, Eigen::MatrixXd>
  calc_f_der(int ib, double *iobs, double *sigo, std::complex<double> *f_calc, double S, const Eigen::VectorXd &ft) const {
    const size_t n_a = rb2a[ib].size();
    Eigen::VectorXd der1 = Eigen::VectorXd::Zero(n_a);
    Eigen::MatrixXd der2 = Eigen::MatrixXd::Zero(n_a, n_a);
    for (int io = 0; io < rb2o[ib].size(); ++io) {
      const int obs_idx = rb2o[ib][io];
      if (std::isnan(iobs[obs_idx]))
        continue;
      const double inv_varobs = 1. / gemmi::sq(sigo[obs_idx]);
      double i_true_twin = 0;
      for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic)
        i_true_twin += alphas[rbo2c[ib][io][ic]] * gemmi::sq(ft(rbo2a[ib][io][ic]));
      for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic) {
        const double tmp = 2 * alphas[rbo2c[ib][io][ic]] * ft(rbo2a[ib][io][ic]);
        der1(rbo2a[ib][io][ic]) -= (iobs[obs_idx] - i_true_twin) * inv_varobs * tmp;
      }
    }
    for (int i = 0; i < n_a; ++i)
      for (int j = i; j < n_a; ++j) {
        for (int io = 0; io < rb2o[ib].size(); ++io) {
          const int obs_idx = rb2o[ib][io];
          if (std::isnan(iobs[obs_idx]))
            continue;
          double i_true_twin = 0;
          for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic)
            i_true_twin += alphas[rbo2c[ib][io][ic]] * gemmi::sq(ft(rbo2a[ib][io][ic]));
          double tmp1 = 0, tmp2 = 0, tmp3 = 0;
          const double inv_varobs = 1. / gemmi::sq(sigo[obs_idx]);
          for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic) {
            const double a_f = 2 * alphas[rbo2c[ib][io][ic]] * ft(rbo2a[ib][io][ic]);
            if (rbo2a[ib][io][ic] == i)
              tmp1 += a_f;
            if (rbo2a[ib][io][ic] == j)
              tmp2 += a_f;
            if (i == j && rbo2a[ib][io][ic] == i)
              tmp3 += 2 * (iobs[obs_idx] - i_true_twin) * alphas[rbo2c[ib][io][ic]];
          }
          // der2(i, j) += (tmp1 * tmp2 - tmp3) * inv_varobs;
          der2(i, j) += (tmp1 * tmp2) * inv_varobs; // should be more stable?
        }
        if (i != j)
          der2(j, i) = der2(i, j);
      }
    for (int ia = 0; ia < n_a; ++ia) {
      const int a_idx = rb2a[ib][ia];
      const int c = centric[a_idx];
      const double inv_den = 1. / (epsilon[a_idx] * S);
      // printf("ia = %d (%d %d %d) c = %d eps= %f S= %f inv_den = %f\n",
      //     ia, asu[a_idx][0], asu[a_idx][1], asu[a_idx][2],
      //     c, epsilon[a_idx], S, inv_den);
      der1(ia) += 2 * ft(ia) * inv_den / (1. + c);
      der2(ia, ia) += 2 * inv_den / (1. + c);
      const double X = std::abs(f_calc[a_idx]) * ft(ia) * inv_den;
      const double m = fom(X, c + 1);
      const double f_inv_den = std::abs(f_calc[a_idx]) * inv_den * (2 - c);
      der1(ia) -= m * f_inv_den;
      der2(ia, ia) -= fom_der(m, X, c + 1) * gemmi::sq(f_inv_den);
      if (c == 0) { // acentric
        der1(ia) -= 1. / ft(ia);
        der2(ia, ia) += 1. / gemmi::sq(ft(ia));
      }
    }
    return std::make_pair(der1, der2);
  }



  // Note that f_calc refers to asu, while iobs/sigo refer to observation list
  double ll_block(int ib, double *iobs, double *sigo, std::complex<double> *f_calc, double S) const {
    if (ib < 0 || ib > rb2o.size())
      throw std::out_of_range("twin_ll: bad ib");
    // skip if no observation at all
    bool has_obs = false;
    for (int io = 0; io < rb2o[ib].size(); ++io)
      if (!std::isnan(iobs[rb2o[ib][io]]))
        has_obs = true;
    if (!has_obs)
      return 0;

    // Initial estimate
    std::vector<std::complex<double>> f_est(rb2a[ib].size());
    for (int io = 0; io < rb2o[ib].size(); ++io) {
      const int obs_idx = rb2o[ib][io];
      if (std::isnan(iobs[obs_idx]))
        continue;
      const double i_obs = std::max(0.001 * sigo[obs_idx], iobs[obs_idx]);
      double i_calc_twin = 0;
      for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic)
        i_calc_twin += alphas[rbo2c[ib][io][ic]] * std::norm(f_calc[rb2a[ib][rbo2a[ib][io][ic]]]);
      // printf("debug: i_obs %f i_calc_twin %f\n", i_obs, i_calc_twin);
      for (int ic = 0; ic < rbo2a[ib][io].size(); ++ic) {
        f_est[rbo2a[ib][io][ic]] += alphas[rbo2c[ib][io][ic]] * std::sqrt(i_obs / i_calc_twin);
        //std::cout << "debug2: f_est[" << rbo2a[ib][io][ic] << "] += " <<  alphas[rbo2c[ib][io][ic]] * std::sqrt(i_obs / i_calc_twin) * f_calc[rb2a[ib][rbo2a[ib][io][ic]]] << "\n";
      }
      //std::cout << "debug3: f_est= " << f_est[0] << "\n";
    }
    Eigen::VectorXd f_true(rb2a[ib].size());
    for (int ia = 0; ia < rb2a[ib].size(); ++ia)
      f_true(ia) = std::abs(f_est[ia] * f_calc[rb2a[ib][ia]]);

    Eigen::VectorXd f_true_old = f_true;
    Eigen::IOFormat Fmt(Eigen::StreamPrecision, Eigen::DontAlignCols, " ", " ", "", "", "", "");
    const double tol = 1.e-5; // enough?
    double det = 1, f0 = 0;
    std::pair<Eigen::VectorXd,Eigen::MatrixXd> ders;
    for (int i_cyc = 0; i_cyc < 100; ++i_cyc) {
      f0 = calc_f(ib, iobs, sigo, f_calc, S, f_true);
      // printf("f = %.e\n", f0);
      // printf("f_der = [");
      ders = calc_f_der(ib, iobs, sigo, f_calc, S, f_true);
      // std::cout << ders.first;
      // printf("]\n");

      const double e = 1e-2;
      // // test der1
      // printf("f_num = [");
      // for (int ia = 0; ia < rb2a[ib].size(); ++ia) {
      //        for (int ia2 = 0; ia2 < rb2a[ib].size(); ++ia2)
      //          f_true(ia2) = f_calc[rb2a[ib][ia2]] + (ia2 == ia ? e : 0);
      //        const double f1 = f(f_true);
      //        printf("%.6e, ", (f1-f0)/e);
      // }
      // printf("\n");
      if (0) {
        std::cout << "f_der2 = \n" << ders.second << "\n";
        std::cout << "f_der2_num = \n";
        for (int ia = 0; ia < rb2a[ib].size(); ++ia) {
          Eigen::VectorXd f_true2 = f_true;
          for (int ia2 = 0; ia2 < rb2a[ib].size(); ++ia2)
            //f_true2(ia2) += (ia2 == ia ? e : 0);
            if (ia2 == ia)
              f_true2(ia2) += e;
          const auto f1_der = calc_f_der(ib, iobs, sigo, f_calc, S, f_true2);
          // std::cout << "debug: ft = " << f_true2.format(Fmt) << "\n"
          //      << "f1_der = " << f1_der.first.format(Fmt) << "\n";
          const auto nder2 = (f1_der.first(ia) - ders.first(ia)) / e;
          printf("num= %.6e ratio= %.5e\n", nder2, nder2/ders.second(ia,ia));
        }
        printf("\n");
      }
      // std::cout << "f_der2_num_ver2 = \n";
      // for (int ia = 0; ia < rb2a[ib].size(); ++ia) {
      //        Eigen::VectorXd f_true2 = f_true;
      //        for (int ia2 = 0; ia2 < rb2a[ib].size(); ++ia2)
      //          f_true2(ia2) += (ia2 == ia ? 2*e : 0);
      //        const double f1_2h = f(f_true2);
      //        f_true2 = f_true;
      //        for (int ia2 = 0; ia2 < rb2a[ib].size(); ++ia2)
      //          f_true2(ia2) += (ia2 == ia ? e : 0);
      //        const double f1_h = f(f_true2);
      //        printf("%.6e, ", (f1_2h - 2 * f1_h + f0)/e/e);
      // }
      // printf("\n");
      Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(ders.second);
      Eigen::VectorXd eig_inv = es.eigenvalues();
      det = es.eigenvalues().prod();

      // Invert
      for (int i = 0; i < eig_inv.size(); ++i)
        eig_inv(i) = std::abs(eig_inv(i)) < 1e-8 ? 1 : (1. / eig_inv(i));
      auto a_inv = es.eigenvectors() * eig_inv.asDiagonal() * es.eigenvectors().adjoint();
      f_true_old = f_true;
      Eigen::VectorXd shift = a_inv * ders.first;
      // std::cout << ib << " " << f_true.format(Fmt)
      //           << " first " << ders.first.format(Fmt)
      //           << " second " << ders.second.format(Fmt)
      //                << " shift " << shift.format(Fmt);

      const double g2p = ders.first.dot(shift);
      const double tol_conv = 1e-6;

      // Line search
      double lambda = 1, lambda_old = 1;
      double f1 = 0, f2 = 0;
      while (((f_true - shift * lambda).array() < 0).any()) {
        if (lambda < 0.1)
          break;
        lambda *= 0.75;
      }
      for (int i_ls = 0; i_ls < 20; ++i_ls) {
        f1 = calc_f(ib, iobs, sigo, f_calc, S, f_true - lambda * shift);
        if (f1 <= f0 - 1e-4 * lambda * g2p)
          break;
        double tmp = 0.5;
        if (i_ls > 0) {
          double l12 = lambda - lambda_old;
          double r1 = f1 - f0 + lambda * g2p;
          double r2 = f2 - f0 + lambda_old * g2p;
          double a = (r1 / gemmi::sq(lambda) - r2 / gemmi::sq(lambda_old)) / l12;
          double b = (-lambda_old * r1 / gemmi::sq(lambda) + lambda * r2 / gemmi::sq(lambda_old)) / l12;
          //printf("debug l12 r1 r2 a b %f %f %f %f %f\n", l12, r1, r2, a, b);
          if (a == 0)
            tmp = g2p / b * 0.5;
          else
            tmp = (-b + std::sqrt(std::max(0., gemmi::sq(b) + 3 * a * g2p))) / 3. / a;
        }
        tmp = std::min(tmp, 0.9 * lambda);
        lambda_old = lambda;
        lambda = std::max(tmp, 0.1 * lambda);
        f2 = f1;
      }
      // std::cout << " lambda " << lambda << "\n";

      if (g2p * lambda / f_true.size() < tol_conv)
        break;

      // //if (ders.first.norm() < tol || shift.norm() < tol) {
      //        std::cout << "\n";
      //        double det = 1;
      //        for (int i = 0; i < es.eigenvalues().size(); ++i)
      //          det *= es.eigenvalues()(i);
      //        return f0 + 0.5 * std::log(det); // Laplace approximation. omitted (2pi)**N/2
      // }

      f_true = (f_true - lambda * shift).cwiseMax(1e-6);
      // XXX f_true should not be negative

      // std::cout << "now = \n" << f_true << "\n";
      // auto ders2 = f_der(f_true);
      // std::cout << "f_der1 = \n" << ders2.first << "\n";
      // std::cout << "f_der2 = \n" << ders2.second << "\n";
    }
    std::cout << "debug: " << ib <<  " "
              << f0 << " " << det << " " << std::log(det) << " "
              << ders.first.format(Fmt) << std::endl;// << f_true.format(Fmt) <<
    //return f0 + 0.5 * std::log(det); // Laplace approximation. omitted (2pi)**N/2

    // calculate Rice distribution using Ftrue as Fobs
    double ret = 0;
    for (int i = 0; i < rb2a[ib].size(); ++i) {
      const int ia = rb2a[ib][i];
      const int c = centric[ia] + 1;
      const double log_ic0 = log_i0_or_cosh(f_true(i) * std::abs(f_calc[ia]) / S, c);
      ret += std::log(S) / c + (sq(f_true(i)) + std::norm(f_calc[ia])) / (S * c) - log_ic0;
    }
    return ret;
    //throw std::runtime_error("did not converge. ib = " + std::to_string(ib));
  }
};

void add_twin(py::module& m) {
  py::class_<TwinData>(m, "TwinData")
    .def(py::init<>())
    .def_readonly("rb2o", &TwinData::rb2o)
    .def_readonly("rb2a", &TwinData::rb2a)
    .def_readonly("rbo2a", &TwinData::rbo2a)
    .def_readonly("rbo2c", &TwinData::rbo2c)
    .def_readonly("rbin", &TwinData::rbin)
    .def_readonly("asu", &TwinData::asu)
    .def_readonly("centric", &TwinData::centric)
    .def_readonly("epsilon", &TwinData::epsilon)
    .def_readonly("bin", &TwinData::bin)
    .def_readwrite("alphas", &TwinData::alphas)
    .def("idx_of_asu", [](const TwinData &self, py::array_t<int> hkl, bool inv){
      auto h = hkl.unchecked<2>();
      if (h.shape(1) < 3)
        throw std::domain_error("error: the size of the second dimension < 3");
      const size_t ret_size = inv ? self.asu.size() : h.shape(0);
      auto ret = py::array_t<int>(ret_size);
      int* ptr = (int*) ret.request().ptr;
      for (py::ssize_t i = 0; i < ret_size; ++i)
        ptr[i] = -1;
      for (py::ssize_t i = 0; i < h.shape(0); ++i) {
        int j = self.idx_of_asu({h(i, 0), h(i, 1), h(i, 2)});
        // if (j >= h.shape(0))
        //   throw std::runtime_error("bad idx_of_asu " +
        //                            std::to_string(h(i,0))+" "+
        //                            std::to_string(h(i,1))+" "+
        //                            std::to_string(h(i,2)));
        if (inv)
          ptr[j] = i;
        else
          ptr[i] = j;
      }
      return ret;
    }, py::arg("hkl"), py::arg("inv")=false)
    .def("setup", [](TwinData &self, py::array_t<int> hkl, const std::vector<int> &bin,
                     const gemmi::SpaceGroup &sg, const std::vector<gemmi::Op> &operators) {
      auto h = hkl.unchecked<2>();
      if (h.shape(1) < 3)
        throw std::domain_error("error: the size of the second dimension < 3");
      std::vector<gemmi::Op::Miller> hkls;
      hkls.reserve(h.shape(0));
      for (py::ssize_t i = 0; i < h.shape(0); ++i)
        hkls.push_back({h(i, 0), h(i, 1), h(i, 2)});
      self.setup(hkls, bin, sg, operators);
    })
    .def("pairs", [](const TwinData &self, int i_op, int i_bin) {
      if (i_op < 0 || i_op >= self.alphas.size())
        throw std::runtime_error("bad i_op");
      std::vector<std::array<size_t, 2>> idxes;
      idxes.reserve(self.rb2o.size());
      for (int ib = 0; ib < self.rb2o.size(); ++ib) {
        if (i_bin >= 0 && self.rbin[ib] != i_bin)
          continue;
        for (int io = 0; io < self.rb2o[ib].size(); ++io)
          for (int io2 = io+1; io2 < self.rb2o[ib].size(); ++io2)
            if (self.rbo2a[ib][io2][0] == self.rbo2a[ib][io][i_op+1] &&
                self.rb2o[ib][io] != self.rb2o[ib][io2])
              idxes.push_back({self.rb2o[ib][io], self.rb2o[ib][io2]});
      }
      return idxes;
    }, py::arg("i_op"), py::arg("i_bin")=-1)
    .def("obs_related_asu", [](const TwinData &self) {
      const size_t n_ops = self.n_ops(); // include identity
      auto ret = py::array_t<int>({self.n_obs(), n_ops});
      int *ptr = (int*) ret.request().ptr;
      for (int ib = 0; ib < self.rb2o.size(); ++ib)
        for (int io = 0; io < self.rb2o[ib].size(); ++io) {
          int *ptr2 = ptr + self.rb2o[ib][io] * n_ops;
          for (int ic = 0; ic < self.rbo2a[ib][io].size(); ++ic)
            ptr2[ic] = self.rb2a[ib][self.rbo2a[ib][io][ic]];
        }
      return ret;
    })
    .def("twin_related", [](const TwinData &self,
                            const gemmi::SpaceGroup &sg, const std::vector<gemmi::Op> &operators) {
      const size_t n_asu = self.asu.size();
      //if (data.shape(0) != n_asu)
      //  throw std::runtime_error("data and asu shapes mismatch");
      const gemmi::GroupOps gops = sg.operations();
      const gemmi::ReciprocalAsu rasu(&sg);
      auto apply_and_asu = [&rasu, &gops](const gemmi::Op &op, const gemmi::Op::Miller &h) {
        return rasu.to_asu(op.apply_to_hkl(h), gops).first;
      };
      const size_t n_ops = operators.size() + 1;
      auto ret = py::array_t<int>({n_asu, n_ops});
      int* ptr = (int*) ret.request().ptr;
      //auto data_ = data.unchecked<1>();
      for (int i = 0; i < n_asu; ++i) {
        const auto h = self.asu[i];
        ptr[i * n_ops] = i; //data_(i);
        for (int j = 1; j < n_ops; ++j) {
          const auto hr = apply_and_asu(operators[j-1], h);
          ptr[i * n_ops + j] = self.idx_of_asu(hr);
        }
      }
      return ret;
    })
    .def("ll_block", [](const TwinData &self, int ib, py::array_t<double> Io, py::array_t<double> sigIo,
                        double S, py::array_t<std::complex<double>> DFc) {

      return self.ll_block(ib,
                           (double*) Io.request().ptr,
                           (double*) sigIo.request().ptr,
                           (std::complex<double>*) DFc.request().ptr, S);
    })
    .def("ll", [](const TwinData &self, py::array_t<double> Io, py::array_t<double> sigIo,
                  py::array_t<double> S, py::array_t<std::complex<double>> DFc) {
      auto S_ = S.unchecked<1>();
      double ret = 0;
      for (size_t ib = 0; ib < self.rb2o.size(); ++ib) {
        const int bin = self.rbin[ib];
        ret += self.ll_block(ib,
                             (double*) Io.request().ptr,
                             (double*) sigIo.request().ptr,
                             (std::complex<double>*) DFc.request().ptr, S_(bin));
      }
      return ret;
    })
    ;
}
