//===- AMDGPUOpenMP.cpp - AMDGPUOpenMP ToolChain Implementation -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AMDGPUOpenMP.h"
#include "AMDGPU.h"
#include "CommonArgs.h"
#include "InputInfo.h"
#include "clang/Basic/Cuda.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetParser.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

namespace {

static void addBCLib(const Driver &D, const ArgList &Args,
                     ArgStringList &CmdArgs, ArgStringList LibraryPaths,
                     StringRef BCName) {
  StringRef FullName;
  for (std::string LibraryPath : LibraryPaths) {
    SmallString<128> Path(LibraryPath);
    llvm::sys::path::append(Path, BCName);
    FullName = Path;
    if (llvm::sys::fs::exists(FullName)) {
      CmdArgs.push_back("-mlink-builtin-bitcode");
      CmdArgs.push_back(Args.MakeArgString(FullName));
      return;
    }
  }
  D.Diag(diag::err_drv_no_such_file) << BCName;
}

static const char *getOutputFileName(Compilation &C, StringRef Base,
                                     const char *Postfix,
                                     const char *Extension) {
  const char *OutputFileName;
  if (C.getDriver().isSaveTempsEnabled()) {
    OutputFileName =
        C.getArgs().MakeArgString(Base.str() + Postfix + "." + Extension);
  } else {
    std::string TmpName =
        C.getDriver().GetTemporaryPath(Base.str() + Postfix, Extension);
    OutputFileName = C.addTempFile(C.getArgs().MakeArgString(TmpName));
  }
  return OutputFileName;
}

static void addOptLevelArgs(const llvm::opt::ArgList &Args,
                            llvm::opt::ArgStringList &CmdArgs,
                            bool IsLlc = false) {
  if (Arg *A = Args.getLastArg(options::OPT_O_Group)) {
    StringRef OOpt = "3";
    if (A->getOption().matches(options::OPT_O4) ||
        A->getOption().matches(options::OPT_Ofast))
      OOpt = "3";
    else if (A->getOption().matches(options::OPT_O0))
      OOpt = "0";
    else if (A->getOption().matches(options::OPT_O)) {
      // Clang and opt support -Os/-Oz; llc only supports -O0, -O1, -O2 and -O3
      // so we map -Os/-Oz to -O2.
      // Only clang supports -Og, and maps it to -O1.
      // We map anything else to -O2.
      OOpt = llvm::StringSwitch<const char *>(A->getValue())
                 .Case("1", "1")
                 .Case("2", "2")
                 .Case("3", "3")
                 .Case("s", IsLlc ? "2" : "s")
                 .Case("z", IsLlc ? "2" : "z")
                 .Case("g", "1")
                 .Default("2");
    }
    CmdArgs.push_back(Args.MakeArgString("-O" + OOpt));
  }
}
} // namespace


const char *AMDGCN::OpenMPLinker::constructLLVMLinkCommand(
    Compilation &C, const JobAction &JA, const InputInfoList &Inputs,
    const ArgList &Args, StringRef SubArchName,
    StringRef OutputFilePrefix) const {
  ArgStringList CmdArgs;

  for (const auto &II : Inputs)
    if (II.isFilename())
      CmdArgs.push_back(II.getFilename());
  // Add an intermediate output file.
  CmdArgs.push_back("-o");
  const char *OutputFileName = getOutputFileName(C, OutputFilePrefix, "-linked", "bc");
  CmdArgs.push_back(OutputFileName);
  const char *Exec =
      Args.MakeArgString(getToolChain().GetProgramPath("llvm-link"));
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), Exec, CmdArgs, Inputs,
      InputInfo(&JA, Args.MakeArgString(OutputFileName))));
  return OutputFileName;
}

const char *AMDGCN::OpenMPLinker::constructOptCommand(
    Compilation &C, const JobAction &JA, const InputInfoList &Inputs,
    const llvm::opt::ArgList &Args, llvm::StringRef SubArchName,
    llvm::StringRef OutputFilePrefix, const char *InputFileName) const {
  // Construct opt command.
  ArgStringList OptArgs;
  // The input to opt is the output from llvm-link.
  OptArgs.push_back(InputFileName);
  // Pass optimization arg to opt.
  addOptLevelArgs(Args, OptArgs);
  OptArgs.push_back("-mtriple=amdgcn-amd-amdhsa");
  OptArgs.push_back(Args.MakeArgString("-mcpu=" + SubArchName));

  for (const Arg *A : Args.filtered(options::OPT_mllvm)) {
    OptArgs.push_back(A->getValue(0));
  }

  OptArgs.push_back("-o");
  const char *OutputFileName =
      getOutputFileName(C, OutputFilePrefix, "-optimized", "bc");
  OptArgs.push_back(OutputFileName);
  const char *OptExec =
      Args.MakeArgString(getToolChain().GetProgramPath("opt"));
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), OptExec, OptArgs, Inputs,
      InputInfo(&JA, Args.MakeArgString(OutputFileName))));
  return OutputFileName;
}

const char *AMDGCN::OpenMPLinker::constructLlcCommand(
    Compilation &C, const JobAction &JA, const InputInfoList &Inputs,
    const llvm::opt::ArgList &Args, llvm::StringRef SubArchName,
    llvm::StringRef OutputFilePrefix, const char *InputFileName,
    bool OutputIsAsm) const {
  // Construct llc command.
  ArgStringList LlcArgs;
  // The input to llc is the output from opt.
  LlcArgs.push_back(InputFileName);
  // Pass optimization arg to llc.
  addOptLevelArgs(Args, LlcArgs, /*IsLlc=*/true);
  LlcArgs.push_back("-mtriple=amdgcn-amd-amdhsa");
  LlcArgs.push_back(Args.MakeArgString("-mcpu=" + SubArchName));
  LlcArgs.push_back(
      Args.MakeArgString(Twine("-filetype=") + (OutputIsAsm ? "asm" : "obj")));

  // Extract all the -m options
  std::vector<llvm::StringRef> Features;
  handleTargetFeaturesGroup(Args, Features,
                            options::OPT_m_amdgpu_Features_Group);

  // Add features to mattr such as xnack
  std::string MAttrString = "-mattr=";
  for (auto OneFeature : Features) {
    MAttrString.append(Args.MakeArgString(OneFeature));
    if (OneFeature != Features.back())
      MAttrString.append(",");
  }
  if (!Features.empty())
    LlcArgs.push_back(Args.MakeArgString(MAttrString));

  for (const Arg *A : Args.filtered(options::OPT_mllvm)) {
    LlcArgs.push_back(A->getValue(0));
  }

  // Add output filename
  LlcArgs.push_back("-o");
  const char *LlcOutputFile =
      getOutputFileName(C, OutputFilePrefix, "", OutputIsAsm ? "s" : "o");
  LlcArgs.push_back(LlcOutputFile);
  const char *Llc = Args.MakeArgString(getToolChain().GetProgramPath("llc"));
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), Llc, LlcArgs, Inputs,
      InputInfo(&JA, Args.MakeArgString(LlcOutputFile))));
  return LlcOutputFile;
}

void AMDGCN::OpenMPLinker::constructLldCommand(
    Compilation &C, const JobAction &JA, const InputInfoList &Inputs,
    const InputInfo &Output, const llvm::opt::ArgList &Args,
    const char *InputFileName) const {
  // Construct lld command.
  // The output from ld.lld is an HSA code object file.
  ArgStringList LldArgs{"-flavor",    "gnu", "--no-undefined",
                        "-shared",    "-o",  Output.getFilename(),
                        InputFileName};

  const char *Lld = Args.MakeArgString(getToolChain().GetProgramPath("lld"));
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), Lld, LldArgs, Inputs,
      InputInfo(&JA, Args.MakeArgString(Output.getFilename()))));
}

// For amdgcn the inputs of the linker job are device bitcode and output is
// object file. It calls llvm-link, opt, llc, then lld steps.
void AMDGCN::OpenMPLinker::ConstructJob(Compilation &C, const JobAction &JA,
                                        const InputInfo &Output,
                                        const InputInfoList &Inputs,
                                        const ArgList &Args,
                                        const char *LinkingOutput) const {
  assert(getToolChain().getTriple().isAMDGCN() && "Unsupported target");

  StringRef GPUArch = Args.getLastArgValue(options::OPT_march_EQ);
  assert(GPUArch.startswith("gfx") && "Unsupported sub arch");

  // Prefix for temporary file name.
  std::string Prefix;
  for (const auto &II : Inputs)
    if (II.isFilename())
      Prefix =
          llvm::sys::path::stem(II.getFilename()).str() + "-" + GPUArch.str();
  assert(Prefix.length() && "no linker inputs are files ");

  // Each command outputs different files.
  const char *LLVMLinkCommand =
      constructLLVMLinkCommand(C, JA, Inputs, Args, GPUArch, Prefix);
  const char *OptCommand = constructOptCommand(C, JA, Inputs, Args, GPUArch,
                                               Prefix, LLVMLinkCommand);
  const char *LlcCommand =
      constructLlcCommand(C, JA, Inputs, Args, GPUArch, Prefix, OptCommand);
  constructLldCommand(C, JA, Inputs, Output, Args, LlcCommand);
}

AMDGPUOpenMPToolChain::AMDGPUOpenMPToolChain(const Driver &D,
                                             const llvm::Triple &Triple,
                                             const ToolChain &HostTC,
                                             const ArgList &Args,
                                             const Action::OffloadKind OK)
    : ROCMToolChain(D, Triple, Args), HostTC(HostTC), OK(OK) {
  // Lookup binaries into the driver directory, this is used to
  // discover the clang-offload-bundler executable.
  getProgramPaths().push_back(getDriver().Dir);
}

void AMDGPUOpenMPToolChain::addClangTargetOptions(
    const llvm::opt::ArgList &DriverArgs, llvm::opt::ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadingKind) const {
  HostTC.addClangTargetOptions(DriverArgs, CC1Args, DeviceOffloadingKind);

  StringRef GpuArch = DriverArgs.getLastArgValue(options::OPT_march_EQ);
  assert(!GpuArch.empty() && "Must have an explicit GPU arch.");
  (void)GpuArch;
  assert((DeviceOffloadingKind == Action::OFK_HIP ||
          DeviceOffloadingKind == Action::OFK_OpenMP) &&
         "Only HIP offloading kinds are supported for GPUs.");
  auto Kind = llvm::AMDGPU::parseArchAMDGCN(GpuArch);
  const StringRef CanonArch = llvm::AMDGPU::getArchNameAMDGCN(Kind);

  CC1Args.push_back("-target-cpu");
  CC1Args.push_back(DriverArgs.MakeArgStringRef(GpuArch));
  CC1Args.push_back("-fcuda-is-device");

  if (DriverArgs.hasFlag(options::OPT_fcuda_approx_transcendentals,
                         options::OPT_fno_cuda_approx_transcendentals, false))
    CC1Args.push_back("-fcuda-approx-transcendentals");

  if (DriverArgs.hasFlag(options::OPT_fgpu_rdc, options::OPT_fno_gpu_rdc,
                         false))
    CC1Args.push_back("-fgpu-rdc");

  StringRef MaxThreadsPerBlock =
      DriverArgs.getLastArgValue(options::OPT_gpu_max_threads_per_block_EQ);
  if (!MaxThreadsPerBlock.empty()) {
    std::string ArgStr =
        std::string("--gpu-max-threads-per-block=") + MaxThreadsPerBlock.str();
    CC1Args.push_back(DriverArgs.MakeArgStringRef(ArgStr));
  }

  if (DriverArgs.hasFlag(options::OPT_fgpu_allow_device_init,
                         options::OPT_fno_gpu_allow_device_init, false))
    CC1Args.push_back("-fgpu-allow-device-init");

  CC1Args.push_back("-fcuda-allow-variadic-functions");

  // Default to "hidden" visibility, as object level linking will not be
  // supported for the foreseeable future.
  if (!DriverArgs.hasArg(options::OPT_fvisibility_EQ,
                         options::OPT_fvisibility_ms_compat) &&
      DeviceOffloadingKind != Action::OFK_OpenMP) {
    CC1Args.append({"-fvisibility", "hidden"});
    CC1Args.push_back("-fapply-global-visibility-to-externs");
  }

  if (DriverArgs.hasArg(options::OPT_nogpulib))
    return;
  ArgStringList LibraryPaths;

  LibraryPaths.push_back(
      DriverArgs.MakeArgString(getDriver().Dir + "/../lib/libdevice"));
  LibraryPaths.push_back(DriverArgs.MakeArgString(getDriver().Dir + "/../lib"));

  std::string LibOmptargetBC =
      DriverArgs.MakeArgString("libomptarget-amdgcn-" + GpuArch + ".bc");
  addBCLib(getDriver(), DriverArgs, CC1Args, LibraryPaths, LibOmptargetBC);

  std::string LibDeviceFile = RocmInstallation.getLibDeviceFile(CanonArch);
  if (LibDeviceFile.empty()) {
    getDriver().Diag(diag::err_drv_no_rocm_device_lib) << 1 << GpuArch;
    return;
  }

  bool DAZ = DriverArgs.hasFlag(options::OPT_fcuda_flush_denormals_to_zero,
                                options::OPT_fno_cuda_flush_denormals_to_zero,
                                getDefaultDenormsAreZeroForTarget(Kind));
  // TODO: Check standard C++ flags?
  bool FiniteOnly = false;
  bool UnsafeMathOpt = false;
  bool FastRelaxedMath = false;
  bool CorrectSqrt = true;
  bool Wave64 = isWave64(DriverArgs, Kind);

  // Add the generic set of libraries.
  RocmInstallation.addCommonBitcodeLibCC1Args(
      DriverArgs, CC1Args, LibDeviceFile, Wave64, DAZ, FiniteOnly,
      UnsafeMathOpt, FastRelaxedMath, CorrectSqrt);
}

llvm::opt::DerivedArgList *AMDGPUOpenMPToolChain::TranslateArgs(
    const llvm::opt::DerivedArgList &Args, StringRef BoundArch,
    Action::OffloadKind DeviceOffloadKind) const {
  DerivedArgList *DAL =
      HostTC.TranslateArgs(Args, BoundArch, DeviceOffloadKind);
  if (!DAL)
    DAL = new DerivedArgList(Args.getBaseArgs());

  const OptTable &Opts = getDriver().getOpts();

  if (DeviceOffloadKind != Action::OFK_OpenMP) {
    for (Arg *A : Args) {
      DAL->append(A);
    }
  }

  if (!BoundArch.empty()) {
    DAL->eraseArg(options::OPT_march_EQ);
    DAL->AddJoinedArg(nullptr, Opts.getOption(options::OPT_march_EQ),
                      BoundArch);
  }

  return DAL;
}

Tool *AMDGPUOpenMPToolChain::buildLinker() const {
  assert(getTriple().isAMDGCN());
  return new tools::AMDGCN::OpenMPLinker(*this);
}

void AMDGPUOpenMPToolChain::addClangWarningOptions(
    ArgStringList &CC1Args) const {
  HostTC.addClangWarningOptions(CC1Args);
}

ToolChain::CXXStdlibType
AMDGPUOpenMPToolChain::GetCXXStdlibType(const ArgList &Args) const {
  return HostTC.GetCXXStdlibType(Args);
}

void AMDGPUOpenMPToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  HostTC.AddClangSystemIncludeArgs(DriverArgs, CC1Args);
}

void AMDGPUOpenMPToolChain::AddIAMCUIncludeArgs(const ArgList &Args,
                                                ArgStringList &CC1Args) const {
  HostTC.AddIAMCUIncludeArgs(Args, CC1Args);
}

SanitizerMask AMDGPUOpenMPToolChain::getSupportedSanitizers() const {
  // The AMDGPUOpenMPToolChain only supports sanitizers in the sense that it
  // allows sanitizer arguments on the command line if they are supported by the
  // host toolchain. The AMDGPUOpenMPToolChain will actually ignore any command
  // line arguments for any of these "supported" sanitizers. That means that no
  // sanitization of device code is actually supported at this time.
  //
  // This behavior is necessary because the host and device toolchains
  // invocations often share the command line, so the device toolchain must
  // tolerate flags meant only for the host toolchain.
  return HostTC.getSupportedSanitizers();
}

VersionTuple
AMDGPUOpenMPToolChain::computeMSVCVersion(const Driver *D,
                                          const ArgList &Args) const {
  return HostTC.computeMSVCVersion(D, Args);
}
