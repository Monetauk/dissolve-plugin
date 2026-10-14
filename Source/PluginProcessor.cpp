#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>

namespace
{
    const float kJumps[10] = { -12.f, -7.f, -5.f, -2.f, 0.f, 0.f, 3.f, 5.f, 7.f, 12.f };
}

DissolveProcessor::DissolveProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout DissolveProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    using R = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto add = [&layout] (const char* id, const char* name, R range, float def)
    {
        layout.add (std::make_unique<F> (juce::ParameterID { id, 1 }, name, range, def));
    };

    add ("size",     "Taille (ms)",            R (5.f, 300.f, 0.f, 0.5f), 60.f);
    add ("density",  "Densite (grains/s)",     R (2.f, 40.f),             20.f);
    add ("position", "Position (1 = recent)",  R (0.f, 1.f),              0.7f);
    add ("pitch",    "Pitch (demi-tons)",      R (-24.f, 24.f, 0.f),      0.f);
    add ("chaos",    "Chaos",                  R (0.f, 1.f),              0.3f);
    add ("filter",   "Filtre par grain",       R (0.f, 1.f),              0.35f);
    add ("width",    "Largeur stereo",         R (0.f, 1.f),              0.7f);
    add ("shape",    "Forme d'enveloppe",      R (0.f, 1.f),              0.3f);
    add ("depth",    "Dissolution : profondeur", R (0.f, 1.f),            0.7f);
    add ("dtime",    "Dissolution : duree (s)", R (3.f, 60.f),            20.f);
    add ("reverb",   "Reverb",                 R (0.f, 1.f),              0.3f);
    add ("mix",      "Mix sec / effet",        R (0.f, 1.f),              0.5f);
    add ("bits",     "8 bit : bits",           R (2.f, 12.f),             8.f);
    add ("srate",    "8 bit : reduction (/N)", R (1.f, 40.f, 0.f, 0.5f),  4.f);
    add ("crushmix", "8 bit : mix",            R (0.f, 1.f),              0.35f);
    add ("output",   "Sortie (dB)",            R (-24.f, 6.f),            0.f);

    layout.add (std::make_unique<B> (juce::ParameterID { "dissolve", 1 }, "Dissoudre", false));
    layout.add (std::make_unique<B> (juce::ParameterID { "freeze", 1 }, "Geler la capture", false));

    return layout;
}

bool DissolveProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DissolveProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    fs = sampleRate;
    ringN = (int) (fs * 4.0);
    ringL.assign ((size_t) ringN, 0.0f);
    ringR.assign ((size_t) ringN, 0.0f);
    writePos = 0;
    writeCount = 0;

    for (auto& g : grains)
        g.active = false;

    const size_t block = (size_t) juce::jmax (samplesPerBlock, 1);
    wetL.assign (block, 0.0f);
    wetR.assign (block, 0.0f);
    rvL.assign (block, 0.0f);
    rvR.assign (block, 0.0f);

    reverb.setSampleRate (fs);
    reverb.reset();
    juce::Reverb::Parameters rp;
    rp.roomSize = 0.8f;
    rp.damping = 0.5f;
    rp.wetLevel = 0.33f;
    rp.dryLevel = 0.0f;
    rp.width = 1.0f;
    rp.freezeMode = 0.0f;
    reverb.setParameters (rp);

    spawnCountdown = 0.0f;
    lastDissolve = false;
    dissolving = false;
    dissolveSamples = 0.0;
    dest = 0.0f;
    holdL = holdR = acc = crackleVal = 0.0f;
    crackleLeft = dropLeft = 0;
}

float DissolveProcessor::readInterp (const std::vector<float>& ring, double absIndex) const
{
    const long long i0 = (long long) std::floor (absIndex);
    const float frac = (float) (absIndex - (double) i0);
    const int a = (int) (((i0 % ringN) + ringN) % ringN);
    const int b = (a + 1) % ringN;
    return ring[(size_t) a] + (ring[(size_t) b] - ring[(size_t) a]) * frac;
}

void DissolveProcessor::spawnGrain (const GrainParams& p)
{
    if (rnd.nextFloat() < dest * 0.75f)
        return;

    Grain* g = nullptr;
    for (auto& cand : grains)
        if (! cand.active) { g = &cand; break; }
    if (g == nullptr)
        return;

    float lenMs = p.sizeMs * (1.0f + (rnd.nextFloat() - 0.5f) * p.chaos * 1.2f) * (1.0f - dest * 0.6f);
    lenMs = juce::jmax (2.0f, lenMs);
    const int len = juce::jmax (8, (int) ((double) lenMs * 0.001 * fs));

    float st = p.pitch - dest * 7.0f;
    if (rnd.nextFloat() < p.chaos * 0.6f)
        st += kJumps[rnd.nextInt (10)];

    const double rate = std::pow (2.0, (double) st / 12.0);
    const double need = (double) len * rate;

    double posn = (double) p.position + ((double) rnd.nextFloat() - 0.5) * (double) p.chaos * 0.3;
    posn = juce::jlimit (0.0, 1.0, posn);
    double back = (1.0 - posn) * (double) ringN * 0.9 + need + 64.0;
    back = juce::jmin (back, (double) ringN - 8.0);

    g->readAbs = (double) writeCount - back;
    g->rate = rate;
    g->age = 0;
    g->len = len;
    g->peakAt = juce::jlimit (1, len - 1, (int) ((float) len * (0.5f - 0.42f * p.shape)));
    g->amp = 1.5f / std::sqrt (juce::jmax (1.0f, p.density * lenMs * 0.001f));

    const float pan = (rnd.nextFloat() * 2.0f - 1.0f) * p.width;
    const float th = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    g->gainL = std::cos (th);
    g->gainR = std::sin (th);

    g->useLp = p.filter > 0.02f;
    if (g->useLp)
    {
        const float r = rnd.nextFloat();
        const float fc = 400.0f + (300.0f + r * r * 9000.0f) * (1.0f - p.filter * 0.6f) * (1.0f - dest * 0.5f);
        g->lpA = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * fc / (float) fs);
        g->lpL = g->lpR = 0.0f;
    }

    g->active = true;
}

void DissolveProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || n <= 0)
        return;

    float* chL = buffer.getWritePointer (0);
    float* chR = buffer.getWritePointer (1);

    if ((int) wetL.size() < n)
    {
        wetL.resize ((size_t) n);
        wetR.resize ((size_t) n);
        rvL.resize ((size_t) n);
        rvR.resize ((size_t) n);
    }

    GrainParams gp;
    gp.sizeMs   = param ("size");
    gp.density  = param ("density");
    gp.position = param ("position");
    gp.pitch    = param ("pitch");
    gp.chaos    = param ("chaos");
    gp.filter   = param ("filter");
    gp.width    = param ("width");
    gp.shape    = param ("shape");

    const float depth    = param ("depth");
    const float dtime    = param ("dtime");
    const float rv       = param ("reverb");
    const float mix      = param ("mix");
    const float bits     = param ("bits");
    const float fac      = param ("srate");
    const float cm       = param ("crushmix");
    const float outGain  = juce::Decibels::decibelsToGain (param ("output"));
    const bool dis       = param ("dissolve") > 0.5f;
    const bool frz       = param ("freeze") > 0.5f;

    if (dis && ! lastDissolve)
    {
        dissolving = true;
        dissolveSamples = 0.0;
    }
    lastDissolve = dis;

    if (! dis)
    {
        dissolving = false;
        dest = 0.0f;
    }
    else if (dissolving)
    {
        dissolveSamples += (double) n;
        dest = juce::jmin (1.0f, (float) (dissolveSamples / (fs * (double) dtime))) * depth;
    }

    const float levels = std::pow (2.0f, bits) * 0.5f;
    const float bn = (12.0f - bits) / 10.0f;
    const float fn = (fac - 1.0f) / 39.0f;
    const float par = (bn + fn) * cm;
    const double interval = fs / (double) juce::jmax (gp.density, 0.5f);

    // Passe 1 : capture, grains, 8 bit
    for (int i = 0; i < n; ++i)
    {
        const float xl = chL[i];
        const float xr = chR[i];

        if (! frz)
        {
            ringL[(size_t) writePos] = xl;
            ringR[(size_t) writePos] = xr;
            if (++writePos >= ringN)
                writePos = 0;
            ++writeCount;
        }

        spawnCountdown -= 1.0f;
        if (spawnCountdown <= 0.0f)
        {
            spawnGrain (gp);
            spawnCountdown += (float) (interval * (1.0 + ((double) rnd.nextFloat() - 0.5) * (double) gp.chaos * 1.5));
        }

        float gl = 0.0f, gr = 0.0f;
        for (auto& g : grains)
        {
            if (! g.active)
                continue;

            if (g.age >= g.len
                || g.readAbs >= (double) writeCount - 2.0
                || g.readAbs < (double) (writeCount - ringN + 4))
            {
                g.active = false;
                continue;
            }

            const float env = g.age < g.peakAt
                                ? (float) g.age / (float) g.peakAt
                                : (float) (g.len - g.age) / (float) (g.len - g.peakAt);

            float sl = readInterp (ringL, g.readAbs);
            float sr = readInterp (ringR, g.readAbs);
            if (g.useLp)
            {
                g.lpL += g.lpA * (sl - g.lpL);
                g.lpR += g.lpA * (sr - g.lpR);
                sl = g.lpL;
                sr = g.lpR;
            }

            const float m = 0.5f * (sl + sr) * env * g.amp;
            gl += m * g.gainL;
            gr += m * g.gainR;

            g.readAbs += g.rate;
            ++g.age;
        }

        // Module 8 bit : reduction de frequence, bits, parasites
        acc += 1.0f;
        if (acc >= fac)
        {
            acc -= fac;
            holdL = gl;
            holdR = gr;
        }
        float cl = std::round (holdL * levels) / levels;
        float cr = std::round (holdR * levels) / levels;

        if (rnd.nextFloat() < par * 0.0008f)
        {
            crackleLeft = 5 + rnd.nextInt (30);
            crackleVal = rnd.nextFloat() * 2.0f - 1.0f;
        }
        if (crackleLeft > 0)
        {
            const float nz = crackleVal * 0.35f * (rnd.nextFloat() * 2.0f - 1.0f);
            cl += nz;
            cr += nz;
            --crackleLeft;
        }
        if (dropLeft > 0)
        {
            cl = 0.0f;
            cr = 0.0f;
            --dropLeft;
        }
        else if (rnd.nextFloat() < par * 0.00012f)
        {
            dropLeft = 100 + rnd.nextInt (700);
        }
        const float hiss = (rnd.nextFloat() * 2.0f - 1.0f) * par * 0.015f;
        cl += hiss;
        cr += hiss;

        wetL[(size_t) i] = gl * (1.0f - cm * 0.85f) + cl * cm * 1.3f;
        wetR[(size_t) i] = gr * (1.0f - cm * 0.85f) + cr * cm * 1.3f;
    }

    // Passe 2 : reverb et melange final
    std::copy (wetL.begin(), wetL.begin() + n, rvL.begin());
    std::copy (wetR.begin(), wetR.begin() + n, rvR.begin());
    reverb.processStereo (rvL.data(), rvR.data(), n);

    for (int i = 0; i < n; ++i)
    {
        const float wl = wetL[(size_t) i] * (1.0f - rv * 0.3f) + rvL[(size_t) i] * rv;
        const float wr = wetR[(size_t) i] * (1.0f - rv * 0.3f) + rvR[(size_t) i] * rv;
        chL[i] = (chL[i] * (1.0f - mix) + wl * mix) * outGain;
        chR[i] = (chR[i] * (1.0f - mix) + wr * mix) * outGain;
    }
}

juce::AudioProcessorEditor* DissolveProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void DissolveProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void DissolveProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DissolveProcessor();
}
