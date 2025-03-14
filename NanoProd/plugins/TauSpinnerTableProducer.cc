/**\class TauSpinnerTableProducer

 Description: Produces FlatTable with TauSpinner weights for H->tau,tau events

 Original Author: D.  Winterbottom (IC)
                  update, M. Bluj (NCBJ)
*/

#include "FWCore/Framework/interface/one/EDProducer.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/EventSetup.h"
#include "FWCore/Concurrency/interface/SharedResourceNames.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "DataFormats/Common/interface/View.h"
#include "DataFormats/HepMCCandidate/interface/GenParticle.h"
#include "DataFormats/NanoAOD/interface/FlatTable.h"

#include "Tauola/Tauola.h"
#include "TauSpinner/SimpleParticle.h"
#include "TauSpinner/tau_reweight_lib.h"

#include <atomic>

class TauSpinnerTableProducer : public edm::one::EDProducer<edm::one::SharedResources> {
public:
  explicit TauSpinnerTableProducer(const edm::ParameterSet &);

  void produce(edm::Event &, const edm::EventSetup &) final;
  void beginJob() final;

private:
  void getBosons(edm::RefVector<edm::View<reco::GenParticle>> &bosons, const edm::View<reco::GenParticle> &parts) const;
  static reco::GenParticleRef getLastCopy(const reco::GenParticleRef &part);
  static void getTaus(reco::GenParticleRefVector &taus, const reco::GenParticle &boson);
  static bool getTauDaughters(reco::GenParticleRefVector &tau_daughters, const reco::GenParticle &tau);
  TauSpinner::SimpleParticle convertToSimplePart(const reco::GenParticle &input_part) const {
    return TauSpinner::SimpleParticle(
        input_part.px(), input_part.py(), input_part.pz(), input_part.energy(), input_part.pdgId());
  }

  static std::vector<std::pair<std::string, double>> nameAndValue(const std::vector<double> &val_vec) {
    std::vector<std::pair<std::string, double>> out;
    for (auto val : val_vec) {
      std::string name = std::to_string(val);
      name.erase(name.find_last_not_of('0') + 1, std::string::npos);
      name.erase(name.find_last_not_of('.') + 1, std::string::npos);
      size_t pos = name.find(".");
      if (pos != std::string::npos)
        name.replace(pos, 1, "p");
      pos = name.find("-");
      if (pos != std::string::npos)
        name.replace(pos, 1, "minus");
      out.push_back(std::make_pair(name, val));
    }
    return out;
  }

  void printModuleInfo(edm::ParameterSet const &config) const {
    std::cout << std::string(78, '-') << "\n";
    std::cout << config.getParameter<std::string>("@module_type") << '/'
              << config.getParameter<std::string>("@module_label") << "\n";
    std::cout << "Input: " << config.getParameter<edm::InputTag>("input").encode() << '\n';
    std::cout << "Branch: " << config.getParameter<std::string>("branch") << '\n';
    std::string thetaStr;
    for (const auto &theta : theta_vec_)
      thetaStr += theta.first + ",";
    std::cout << "Theta: " << thetaStr << std::endl;
  }

  const edm::EDGetTokenT<edm::View<reco::GenParticle>> genPartsToken_;
  const std::string branch_;
  const std::vector<std::pair<std::string, double>> theta_vec_;
  const int bosonPdgId_;
  const std::string tauSpinnerPDF_;
  const bool ipp_;
  const int ipol_;
  const int nonSM2_;
  const int nonSMN_;
  const double cmsE_;
  const double default_weight_;

  std::atomic<unsigned int> nWarnings{0};
  static const unsigned int nMaxWarnings = 10;
};

TauSpinnerTableProducer::TauSpinnerTableProducer(const edm::ParameterSet &config)
    : genPartsToken_(consumes(config.getParameter<edm::InputTag>("input"))),
      branch_(config.getParameter<std::string>("branch")),
      theta_vec_(nameAndValue(config.getParameter<std::vector<double>>("theta"))),
      bosonPdgId_(25),                                             //Higgs, to be configurable?
      tauSpinnerPDF_(config.getParameter<std::string>("pdfSet")),  //"NNPDF30_nlo_as_0118"
      ipp_(true),                                                  //pp collisions
      ipol_(0),
      nonSM2_(0),
      nonSMN_(0),
      cmsE_(config.getParameter<double>("cmsE")),  //cms energy in GeV, 13000.0
      default_weight_(1)  //default weight stored in case of presence of a tau decay unsupported by TauSpinner
{
  printModuleInfo(config);

  //state that we use tauola/tauspinner resource
  usesResource(edm::SharedResourceNames::kTauola);

  produces<nanoaod::FlatTable>();
}

void TauSpinnerTableProducer::getBosons(edm::RefVector<edm::View<reco::GenParticle>> &bosons,
                                        const edm::View<reco::GenParticle> &parts) const {
  unsigned idx = 0;
  for (const auto &part : parts) {
    if (std::abs(part.pdgId()) == bosonPdgId_ && part.isLastCopy()) {
      edm::Ref<edm::View<reco::GenParticle>> partRef(&parts, idx);
      bosons.push_back(partRef);
    }
    ++idx;
  }
}

reco::GenParticleRef TauSpinnerTableProducer::getLastCopy(const reco::GenParticleRef &part) {
  if (part->statusFlags().isLastCopy())
    return part;
  for (const auto &daughter : part->daughterRefVector()) {
    if (daughter->pdgId() == part->pdgId() && daughter->statusFlags().fromHardProcess()) {
      return getLastCopy(daughter);
    }
  }
  throw std::runtime_error("getLastCopy: no last copy found");
}

void TauSpinnerTableProducer::getTaus(reco::GenParticleRefVector &taus, const reco::GenParticle &boson) {
  for (const auto &daughterRef : boson.daughterRefVector()) {
    if (std::abs(daughterRef->pdgId()) == 15)
      taus.push_back(getLastCopy(daughterRef));
  }
}

bool TauSpinnerTableProducer::getTauDaughters(reco::GenParticleRefVector &tau_daughters, const reco::GenParticle &tau) {
  static const std::set<int> directTauProducts = {11, 12, 13, 14, 16, 22};
  static const std::set<int> finalHadrons = {111, 130, 211, 310, 311, 321};
  static const std::set<int> intermediateHadrons = {221, 223, 323};
  for (auto daughterRef : tau.daughterRefVector()) {
    const int daughter_pdgId = std::abs(daughterRef->pdgId());
    if ((std::abs(tau.pdgId()) == 15 && directTauProducts.count(daughter_pdgId)) ||
        finalHadrons.count(daughter_pdgId)) {
      tau_daughters.push_back(daughterRef);
    } else if (intermediateHadrons.count(daughter_pdgId)) {
      if (!getTauDaughters(tau_daughters, *daughterRef))
        return false;
    } else {
      edm::LogWarning("TauSpinnerTableProducer::getTauDaughters")
          << "Unsupported decay with " << daughter_pdgId << " being daughter of " << std::abs(tau.pdgId()) << "\n";
      return false;
    }
  }
  return true;
}

void TauSpinnerTableProducer::beginJob() {
  // Initialize TauSpinner
  Tauolapp::Tauola::setNewCurrents(0);
  Tauolapp::Tauola::initialize();
  LHAPDF::initPDFSetByName(tauSpinnerPDF_);
  TauSpinner::initialize_spinner(ipp_, ipol_, nonSM2_, nonSMN_, cmsE_);
}

void TauSpinnerTableProducer::produce(edm::Event &event, const edm::EventSetup &setup) {
  // Input gen-particles collection
  auto const &genParts = event.get(genPartsToken_);

  // Output table
  auto wtTable = std::make_unique<nanoaod::FlatTable>(1, branch_, true);
  wtTable->setDoc("TauSpinner weights");

  // Search for boson
  edm::RefVector<edm::View<reco::GenParticle>> bosons;
  getBosons(bosons, genParts);
  if (bosons.size() != 1) {  //no boson found or more than one found, produce empty table (expected for non HTT sample)
    if(++nWarnings < nMaxWarnings)
      edm::LogWarning("TauSpinnerTableProducer::produce")
	<< "Current event has "<< bosons.size() << " Higgs bosons while there must be exactly one; table with default weights is produced.\n";
    // Fill table with default values
    for (const auto &theta : theta_vec_) {
      wtTable->addColumnValue<double>("weight_cp_" + theta.first,
				      default_weight_,
				      "Dummy TauSpinner weight for theta_CP = " + theta.first);
      wtTable->addColumnValue<double>("weight_cp_" + theta.first + "_alt",
				      default_weight_,
				      "Dummy TauSpinner weight for theta_CP = " + theta.first+ " (alternative hadronic currents)");
    }
    event.put(std::move(wtTable));
    return;
  }

  // Search for taus from boson decay
  reco::GenParticleRefVector taus;
  getTaus(taus, *bosons[0]);
  if (taus.size() != 2) {  //boson does not decay to tau pair, produce empty table (expected for non HTT sample)
    if(++nWarnings < nMaxWarnings)
      edm::LogWarning("TauSpinnerTableProducer::produce")
	<< "Current event has "<< taus.size() << " taus from boson decay while there must be exactly one pair; table with default weights is produced.\n";
    // Fill table with default values
    for (const auto &theta : theta_vec_) {
      wtTable->addColumnValue<double>("weight_cp_" + theta.first,
				      default_weight_,
				      "Dummy TauSpinner weight for theta_CP = " + theta.first);
      wtTable->addColumnValue<double>("weight_cp_" + theta.first + "_alt",
				      default_weight_,
				      "Dummy TauSpinner weight for theta_CP = " + theta.first+ " (alternative hadronic currents)");
    }
    event.put(std::move(wtTable));
    return;
  }

  // Get tau daughters and convert all particles to TauSpinner format
  TauSpinner::SimpleParticle simple_boson = convertToSimplePart(*bosons[0]);
  std::array<TauSpinner::SimpleParticle, 2> simple_taus;
  std::array<std::vector<TauSpinner::SimpleParticle>, 2> simple_tau_daughters;
  bool supportedDecays = true;
  for (size_t tau_idx = 0; tau_idx < 2; ++tau_idx) {
    simple_taus[tau_idx] = convertToSimplePart(*taus[tau_idx]);
    reco::GenParticleRefVector tau_daughters;
    supportedDecays &= getTauDaughters(tau_daughters, *taus[tau_idx]);
    for (const auto &daughterRef : tau_daughters)
      simple_tau_daughters[tau_idx].push_back(convertToSimplePart(*daughterRef));
  }

  // Compute TauSpinner weights and fill table
  std::array<double, 2> weights;
  for (const auto &theta : theta_vec_) {
    // Can make this more general by having boson pdgid as input or have option for set boson type
    TauSpinner::setHiggsParametersTR(-cos(2 * M_PI * theta.second),
                                     cos(2 * M_PI * theta.second),
                                     -sin(2 * M_PI * theta.second),
                                     -sin(2 * M_PI * theta.second));
    for (size_t i = 0; i < weights.size(); ++i) {
      Tauolapp::Tauola::setNewCurrents(i);
      weights[i] =
          supportedDecays
              ? TauSpinner::calculateWeightFromParticlesH(
                    simple_boson, simple_taus[0], simple_taus[1], simple_tau_daughters[0], simple_tau_daughters[1])
              : default_weight_;
    }
    // Nominal weights for setNewCurrents(0)
    wtTable->addColumnValue<double>(
        "weight_cp_" + theta.first, weights[0], "TauSpinner weight for theta_CP=" + theta.first);
    // Weights for alternative hadronic currents (can be used for uncertainty estimates)
    wtTable->addColumnValue<double>(
        "weight_cp_" + theta.first + "_alt",
        weights[1],
        "TauSpinner weight for theta_CP=" + theta.first + " (alternative hadronic currents)");
  }

  event.put(std::move(wtTable));
}

#include "FWCore/Framework/interface/MakerMacros.h"
DEFINE_FWK_MODULE(TauSpinnerTableProducer);
