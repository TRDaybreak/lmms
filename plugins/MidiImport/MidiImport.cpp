/*
 * MidiImport.cpp - support for importing MIDI files
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "MidiImport.h"

#include <array>
#include <sstream>

#include <QDir>
#include <QCoreApplication>
#include <QMessageBox>
#include <QProgressDialog>

#include "InstrumentTrack.h"
#include "AutomationTrack.h"
#include "AutomationPattern.h"
#include "Pattern.h"
#include "Instrument.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "Song.h"
#include "embed.h"
#include "plugin_export.h"
#include "portsmf/allegro.h"

using std::array;
using std::string;
using std::stringstream;

//! Make a four-byte ID value from four sequential one-byte values
static inline constexpr uint32_t makeID(
		uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3)
{
	return c0 | (c1 << 8) | (c2 << 16) | (c3 << 24);
}

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT midiimport_plugin_descriptor =
{
	STRINGIFY(PLUGIN_NAME),
	"MIDI Import",
	QT_TRANSLATE_NOOP("pluginBrowser",
		"Filter for importing MIDI-files into LMMS"),
	"Tobias Doerffel <tobydox/at/users/dot/sf/dot/net>",
	0x0100,
	Plugin::ImportFilter,
	NULL,
	NULL,
	NULL
} ;

} // extern "C"

/*---------------------------------------------------------------------------*/

MidiImport::MidiImport(const QString &filename) :
	ImportFilter(filename, &midiimport_plugin_descriptor) {}

bool MidiImport::tryImport(TrackContainer *tc)
{
	// Try to open file for reading
	bool ok = openFile();
	if (!ok) { return false; }

	if (gui)
	{
#ifdef LMMS_HAVE_FLUIDSYNTH
		// Check if a default soundfount is configured
	 	if (ConfigManager::inst()->sf2File().isEmpty())
		{
			QMessageBox::information(gui->mainWindow(),
					tr("Setup incomplete"),
					tr("You have not set up a default soundfont in "
						"the settings dialog (Edit->Settings). "
						"Therefore no sound will be played back after "
						"importing this MIDI file. You should download "
						"a General MIDI soundfont, specify it in "
						"settings dialog and try again."));
		}
#else
		// No compiled Sf2 support
		QMessageBox::information(gui->mainWindow(),
				tr("Setup incomplete"),
				tr("You did not compile LMMS with support for "
					"SoundFont2 player, which is used to add default "
					"sound to imported MIDI files. "
					"Therefore no sound will be played back after "
					"importing this MIDI file."));
#endif
	}

	// Read first four bytes
	uint32_t id = readID();
	switch (id)
	{
	case makeID('M', 'T', 'h', 'd'):
	{
		// MIDI header found
		printf("MidiImport::tryImport(): found MThd\n");
		return readSMF(*tc);
	}
	case makeID('R', 'I', 'F', 'F'):
	{
		// RIFF header found
		printf("MidiImport::tryImport(): found RIFF\n");
		return readRIFF(*tc);
	}
	default:
	{
		// Not a MIDI file
		puts("MidiImport::tryImport(): not a Standard MIDI file");
		return false;
	}
	}
}

/*---------------------------------------------------------------------------*/

//! Represents and encapsulates an CC automation track
class SmfMidiCC
{
public:
	//! Track where CC automation will be added
	AutomationTrack *track = nullptr;

	//! Last created pattern
	AutomationPattern *pattern = nullptr;

	//! Time/position of last created pattern
	MidiTime lastPos = 0;

	//! Create an automation CC track with optional name
	void create(TrackContainer &tc, const QString &name)
	{
		// Keep LMMS responsive, for now the import runs in the main thread.
		// This should probably be removed if that ever changes
		qApp->processEvents();
		track = dynamic_cast<AutomationTrack *>(
				Track::create(Track::AutomationTrack, &tc));
		if (name != "") { track->setName(name); }
	}

	//! Add an automatable model value at the given time,
	//! creating a new pattern if viable
	void putValue(AutomatableModel &objModel, MidiTime time, double value)
	{
		if (!pattern || (time > lastPos + DefaultTicksPerBar))
		{
			// Mew pattern if none exists, or if far enough from last one
			TrackContentObject *tco = track->createTCO(0);
			pattern = dynamic_cast<AutomationPattern *>(tco);
			MidiTime patPos = MidiTime(time.getBar(), 0);
			pattern->movePosition(patPos);
			pattern->addObject(&objModel);
		}
		// Update current pattern position
		lastPos = time;

		// Add an automated value to pattern
		time -= pattern->startPosition();
		pattern->putValue(time, value, false);
		MidiTime len = MidiTime(time.getBar() + 1, 0);
		pattern->changeLength(len);
	}

	//! Clear values, reset pointers
	void clear()
	{
		track = nullptr;
		pattern = nullptr;
		lastPos = 0;
	}
};

/*---------------------------------------------------------------------------*/

//! Represents and encapsulates a MIDI instrument track
class SmfMidiChannel
{
public:
	//! Track where notes will be added
	InstrumentTrack *track = nullptr;

	//! Last created pattern
	Pattern *pattern = nullptr;

	//! Instrument to be used by pattern (default: Sf2)
	Instrument *inst = nullptr;

	//! The track name
	QString trackName;

	//! If the instrument is from Sf2 Player plugin
	bool isSf2 = false;

	//! If track has at least one note
	bool hasNotes = false;

	//! Create an instrument track with optional name
	void create(TrackContainer &tc, const QString &name)
	{
		// Keep LMMS responsive
		qApp->processEvents();
		track = dynamic_cast<InstrumentTrack *>(
				Track::create(Track::InstrumentTrack, &tc));

#ifdef LMMS_HAVE_FLUIDSYNTH
		inst = track->loadInstrument("sf2player");
		if (inst)
		{
			// Sf2 OK, so create default inst with default patch and bank
			isSf2 = true;
			inst->loadFile(ConfigManager::inst()->sf2File());
			inst->childModel("bank")->setValue(0);
			inst->childModel("patch")->setValue(0);
		}
		else
		{
			inst = track->loadInstrument("patman");
		}
#else
		// Use PatMan if no Sf2 support
		inst = track->loadInstrument("patman");
#endif
		// Set track name
		trackName = name;
		if (trackName != "") { track->setName(name); }

		// General MIDI default
		track->pitchRangeModel()->setInitValue(2);

		// Create a default pattern
		pattern = dynamic_cast<Pattern *>(track->createTCO(0));
	}

	//! Add a single note to pattern and register it
	void addNote(Note &note)
	{
		TrackContentObject *tco = track->createTCO(0);
		if (!pattern) {	pattern = dynamic_cast<Pattern *>(tco); }
		pattern->addNote(note, false);
		hasNotes = true;
	}

	//! Split single pattern into several ones, where viable
	void splitPattern()
	{
		Pattern *newPattern = nullptr;
		MidiTime lastEnd = 0;

		// Sort and iterate through track notes
		pattern->rearrangeAllNotes();
		for (Note *note : pattern->notes())
		{
			if (!newPattern || (note->pos() > lastEnd + DefaultTicksPerBar))
			{
				// New pattern if none exists, or if far enough from last one
				MidiTime patPos = MidiTime(note->pos().getBar(), 0);
				newPattern = dynamic_cast<Pattern*>(track->createTCO(0));
				newPattern->movePosition(patPos);
			}
			// Update end of current note
			lastEnd = note->pos() + note->length();

			// Add new note
			Note newNote = *note;
			newNote.setPos(note->pos(newPattern->startPosition()));
			newPattern->addNote(newNote, false);
		}
		// Get rid of old pattern data
		delete pattern;
		pattern = nullptr;
	}
};

/*---------------------------------------------------------------------------*/

bool MidiImport::readSMF(TrackContainer &tc)
{
	// Number of progress steps before track steps
	static constexpr int PRE_TRACK_STEPS = 2;

	// Set progress dialog
	QProgressDialog pd(TrackContainer::tr("Importing MIDI-file..."),
			TrackContainer::tr("Cancel"),
			0, PRE_TRACK_STEPS, gui->mainWindow());
	pd.setWindowTitle(TrackContainer::tr("Please wait..."));
	pd.setWindowModality(Qt::WindowModal);
	pd.setMinimumDuration(0);
	pd.setValue(0);

	// Build stream from raw file data
	stringstream stream;
	QByteArray arr = readAllData();
	stream.str(string(arr.constData(), arr.size()));

	// Get Allegro sequence of beats from stream
	Alg_seq seq(stream, true);
	seq.convert_to_beats();

	// Add number of tracks to # of progress steps; first one done
	pd.setMaximum(PRE_TRACK_STEPS + seq.tracks());
	pd.setValue(1);

	// Arrays of CCs (128 + Pitch bend) and channels
	array<SmfMidiCC, 129> ccs;
	array<SmfMidiChannel, 256> channels;

	// Add both a numerator and denominator automation tracks
	// with patterns for time signature change (patterns must
	// be pointers or else they disappear when exiting scope)
	MeterModel &timeSigMM = Engine::getSong()->getTimeSigModel();
	AutomationTrack &numeratorTrack = *dynamic_cast<AutomationTrack *>(
			Track::create(Track::AutomationTrack, Engine::getSong()));
	numeratorTrack.setName(tr("MIDI Time Signature Numerator"));
	AutomationTrack &denominatorTrack = *dynamic_cast<AutomationTrack *>(
			Track::create(Track::AutomationTrack, Engine::getSong()));
	denominatorTrack.setName(tr("MIDI Time Signature Denominator"));
	AutomationPattern *timeSigNumeratorPat =
			new AutomationPattern(&numeratorTrack);
	timeSigNumeratorPat->setDisplayName(tr("Numerator"));
	timeSigNumeratorPat->addObject(&timeSigMM.numeratorModel());
	AutomationPattern *timeSigDenominatorPat =
			new AutomationPattern(&denominatorTrack);
	timeSigDenominatorPat->setDisplayName(tr("Denominator"));
	timeSigDenominatorPat->addObject(&timeSigMM.denominatorModel());

	// TODO: adjust these to Time.Sig changes
	double beatsPerBar = 4;
	double ticksPerBeat = DefaultTicksPerBar / beatsPerBar;

	// Register correct time signature from MIDI file events
	Alg_time_sigs &timeSigs = seq.time_sig;
	for (size_t i = 0; i < timeSigs.length(); ++i)
	{
		Alg_time_sig &timeSig = timeSigs[i];
		timeSigNumeratorPat->putValue(
				timeSig.beat * ticksPerBeat, timeSig.num);
		timeSigDenominatorPat->putValue(
				timeSig.beat * ticksPerBeat, timeSig.den);
	}
	// Manually update (otherwise pattern shows being 1 bar)
	timeSigNumeratorPat->updateLength();
	timeSigDenominatorPat->updateLength();

	// Pre track steps done
	pd.setValue(2);

	// Automate tempo based on time difference between song beats
	AutomationPattern *tempoAutoPat = tc.tempoAutomationPattern();
	if (tempoAutoPat)
	{
		tempoAutoPat->clear();
		Alg_time_map &timeMap = *seq.get_time_map();
		Alg_beats &beats = timeMap.beats;
		for (size_t i = 0; i < beats.len - 1; ++i)
		{
			Alg_beat &beat = beats[i];
			Alg_beat &beatNext = beats[i+1];
			double tempo = (beatNext.beat - beat.beat)
					/ (beatNext.time - beat.time);
			double bpm = tempo * 60.0;
			tempoAutoPat->putValue(beat.beat * ticksPerBeat, bpm);
		}
		if (timeMap.last_tempo_flag)
		{
			Alg_beat &beat = beats[beats.len - 1];
			double bpm = timeMap.last_tempo * 60.0;
			tempoAutoPat->putValue(beat.beat * ticksPerBeat, bpm);
		}
	}
	// Update the tempo to avoid crash when playing a project imported
	// via the command line
	Engine::updateFramesPerTick();

	// Check for unhandled song updates
	for (size_t i = 0; i < seq.length(); ++i)
	{
		Alg_event &event = *seq[i];
		if (event.is_update())
		{
			fprintf(stderr, "Unhandled SONG update: %d %f %s\n",
					event.get_type_code(), event.time, event.get_attribute());
		}
	}

	// Iterate through tracks
	for (size_t i = 0; i < seq.tracks(); ++i)
	{
		// Get track and track name
		Alg_track &track = *seq.track(i);
		QString trackName = QString(tr("Track") + "%1").arg(i);
		pd.setValue(pd.value() + 1);

		// Clear all CCs
		for (size_t j = 0; j < ccs.size(); ++j) { ccs[j].clear(); }

		// Now look at track events
		for (size_t j = 0; j < track.length(); ++j)
		{
			Alg_event &event = *track[j];
			if (event.chan == -1)
			{
				// Handle special events
				bool handled = false;
                if (event.is_update())
				{
					// Set track name string, if available
					QString attr = event.get_attribute();
                    if (attr == "tracknames" && event.get_update_type() == 's') {
						trackName = event.get_string_value();
						handled = true;
					}
				}
                if (!handled) {
                    // Write debug output
                    fprintf(stderr, "MISSING GLOBAL HANDLER\n");
                    fprintf(stderr, "\tChannel: %ld, Type Code: %d, Time: %f",
							event.chan, event.get_type_code(), event.time);
                    if (event.is_update())
                    {
                        fprintf(stderr, ", Update Type: %s", event.get_attribute());
                        if (event.get_update_type() == 'a')
                        {
                            printf(", Atom: %s", event.get_atom_value());
                        }
                    }
                    printf("\n");
				}
			}
			else if (event.is_note() && event.chan < channels.size())
			{
				// Create channel if none
				SmfMidiChannel &channel = channels[event.chan];
				if (!channel.track) { channel.create(tc, trackName); }

				// Process and create note event, add it to channel
				// (volume goes from MIDI velocity to LMMS volume)
				Alg_note &noteEvent = *dynamic_cast<Alg_note *>(&event);
				int ticks = noteEvent.get_duration() * ticksPerBeat;
				ticks = qMax(1, ticks);
				int pos = noteEvent.get_start_time() * ticksPerBeat;
				int key = noteEvent.get_identifier() - 12;
				volume_t volume = noteEvent.get_loud() * (200.0 / 127.0);
				Note note(ticks, pos, key, volume);
				channel.addNote(note);
			}
			else if (event.is_update())
			{
				// Create channel if none
				SmfMidiChannel &channel = channels[event.chan];
				if (!channel.track) {channel.create(tc, trackName); }

				// Check for update events
				QString updateName = event.get_attribute();
				double time = event.time * ticksPerBeat;
				if (updateName == "programi")
				{
					long prog = event.get_integer_value();
					if (channel.isSf2)
					{
						// Program change equals the soundfont patch number
						channel.inst->childModel("bank")->setValue(0);
						channel.inst->childModel("patch")->setValue(prog);
					}
					else {
						// TODO: Check if DIR exists?
						static const QString DIR = "/usr/share/midi/freepats/Tone_000/";
						QString num = QString::number(prog);
						QString filter = QString().fill('0', 3 - num.length()) + num + "*.pat";
						QStringList files = QDir(DIR).
						entryList(QStringList(filter));
						if (channel.inst && !files.empty())
						{
							QString file = DIR + files.front();
							channel.inst->loadFile(file);
						}
					}
				}
				else if (updateName.startsWith("control") || updateName == "bendr")
				{
					// ccId is 128 for bendr, or the XXX in "controlXXX"
					int ccId = 128;
					if (updateName != "bendr")
					{
						ccId = updateName.mid(7, updateName.length() - 8).toInt();
					}
					if (ccId <= 128)
					{
						// Check for automation updates
						double val = event.get_real_value();
						AutomatableModel *objModel = nullptr;
						switch (ccId)
						{
						case 0:
						{
							// Bank update
							if (channel.isSf2 && channel.inst)
							{
								objModel = channel.inst->childModel("bank");
								fprintf(stderr, "BANK SELECT %f %d\n", val, (int) (127 * val));
								val *= 127;
							}
							break;
						}
						case 7:
						{
							// Volume update
							objModel = channel.track->volumeModel();
							fprintf(stderr, "VOLUME SELECT %f %d\n", val, (int) (100 * val));
							val *= 100;
							break;
						}
						case 10:
						{
							// Pan update
							objModel = channel.track->panningModel();
							fprintf(stderr, "PAN SELECT %f %d\n", val, (int) ((200 * val) - 100));
							val = (200 * val) - 100;
							break;
						}
						case 128:
						{
							// Key pitch update
							objModel = channel.track->pitchModel();
							fprintf(stderr, "KEY SELECT %f %d\n", val, (int) (100 * val));
							val = 100 * val;
							break;
						}
						default:
						{
							//TODO: something useful for other CCs
							break;
						}
						}
						if (objModel)
						{
							// When at time 0, just change global settings
							if (time == 0) { objModel->setInitValue(val); }
							else
							{
								// Else, create a track if none and put a pattern on it
								if (!ccs[ccId].track)
								{
									QString name = trackName + " > "
											+ (objModel ? objModel->displayName()
												: QString("CC %1").arg(ccId));
									ccs[ccId].create(tc, name);
								}
								ccs[ccId].putValue(*objModel, time, val);
							}
						}
					}
				}
				else
				{
					// Something not recognized
					fprintf(stderr, "Unhandled update: %d %d %f %s\n", (int) event.chan,
							event.get_type_code(), event.time, event.get_attribute());
				}
			}
		}
	}
	// Split track patterns and remove empty tracks
	for (size_t i = 0; i < channels.size(); ++i)
	{
		if (channels[i].hasNotes) { channels[i].splitPattern(); }
		else if (channels[i].track)
		{
			printf(" Should remove empty track\n");
			// TODO: must delete trackView first - but where is it?
			//tc->removeTrack(chs[c].it);
			//it->deleteLater();
		}
	}
	// Set channel 10 to drums as per General MIDI's orders
	if (channels[9].hasNotes && channels[9].inst && channels[9].isSf2)
	{
		// AFAIK, 128 should be the standard bank for drums in SF2.
		// If not, this has to be made configurable.
		channels[9].inst->childModel("bank")->setValue(128);
		channels[9].inst->childModel("patch")->setValue(0);
	}

	// Everything's fine
	return true;
}

/*---------------------------------------------------------------------------*/

bool MidiImport::readRIFF(TrackContainer &tc)
{
	// Skip file length
	skip(4);

	// Check file type ("RMID" = RIFF MIDI)
	if (readID() != makeID('R', 'M', 'I', 'D'))
	{
		qWarning("MidiImport::readRIFF(): invalid file format");
		return false;
	}
	// Search for "data" chunk
	while (true)
	{
		uint32_t id = readID();
		if (file().atEnd())
		{
			qWarning("MidiImport::readRIFF(): data chunk not found");
			return false;
		}
		if (id == makeID('d', 'a', 't', 'a')) {	break; }

		uint32_t len = read32LE();
		if (len < 0)
		{
			qWarning("MidiImport::readRIFF(): data chunk not found");
			return false;
		}
		skip((len + 1) & ~1);
	}

	// The "data" chunk must contain data in SMF format
	if (readID() != makeID( 'M', 'T', 'h', 'd' ))
	{
		qWarning("MidiImport::readRIFF(): invalid file format");
		return false;
	}
	return readSMF(tc);
}

/*---------------------------------------------------------------------------*/

extern "C"
{

//! Necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main(Model *, void *data)
{
	QString filename = QString::fromUtf8(static_cast<const char *>(data));
	return new MidiImport(filename);
}

} // extern "C"
