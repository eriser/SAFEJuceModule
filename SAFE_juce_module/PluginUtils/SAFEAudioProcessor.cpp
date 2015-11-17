//==========================================================================
//      A Class to Put the Analysis on a Separate Thread
//==========================================================================
//==========================================================================
//      Constructor and Destructor
//==========================================================================
SAFEAudioProcessor::AnalysisThread::AnalysisThread (SAFEAudioProcessor* processorInit)
    : Thread ("AnalysisThread")
{
    processor = processorInit;
}

SAFEAudioProcessor::AnalysisThread::~AnalysisThread()
{
    stopThread (4000);
}

//==========================================================================
//      The Thread Callback
//==========================================================================
void SAFEAudioProcessor::AnalysisThread::run()
{
    GenericScopedLock <SpinLock> lock (mutex);

    WarningID warning;

    if (sendToServer)
    {
        warning = processor->sendDataToServer (descriptors, metaData);
    }
    else
    {
        warning = processor->saveSemanticData (descriptors, metaData);
    }

    if (warning != NoWarning)
    {
        processor->sendWarningToEditor (warning);
    }

    processor->readyToSave = true;
}

//==========================================================================
//      Set Some Parameters
//==========================================================================
void SAFEAudioProcessor::AnalysisThread::setParameters (String newDescriptors, SAFEMetaData newMetaData, bool newSendToServer)
{
    descriptors = newDescriptors;
    metaData = newMetaData;
    sendToServer = newSendToServer;
}

//==========================================================================
//      Lock to ensure only one plug-in saves at a time
//==========================================================================
SpinLock SAFEAudioProcessor::AnalysisThread::mutex;

//==========================================================================
//      The Processor Itself
//==========================================================================
//==========================================================================
//      Constructor and Destructor
//==========================================================================
SAFEAudioProcessor::SAFEAudioProcessor()
{
    // reset tap values
    unprocessedTap = processedTap = 0;

    // get the semantic data file set up
    initialiseSemanticDataFile();

    playHead.resetToDefault();

    recording = false;
    readyToSave = true;

    numInputs = 1;
    numOutputs = 1;

    analysisThread = new AnalysisThread (this);

    controlRate = 64;
    controlBlockSize = (int) (44100.0 / controlRate);
    remainingControlBlockSamples = 0;
}

SAFEAudioProcessor::~SAFEAudioProcessor()
{
}

//==========================================================================
//      Parameter Info Methods
//==========================================================================
const String SAFEAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

int SAFEAudioProcessor::getNumParameters()
{
    return parameters.size();
}

float SAFEAudioProcessor::getParameter (int index)
{
    return parameters [index]->getBaseValue();
}

void SAFEAudioProcessor::setParameter (int index, float newValue)
{
    parameters [index]->setBaseValue (newValue);
    
    parameterUpdateCalculations (index);
}

float SAFEAudioProcessor::getScaledParameter (int index)
{
    return parameters [index]->getScaledValue();
}

void SAFEAudioProcessor::setScaledParameter (int index, float newValue)
{
    parameters [index]->setScaledValue (newValue);
    
    parameterUpdateCalculations (index);
}

void SAFEAudioProcessor::setScaledParameterNotifyingHost (int index, float newValue)
{
    setScaledParameter (index, newValue);
    float newBaseValue = parameters [index]->getBaseValue();
    sendParamChangeMessageToListeners (index, newBaseValue);
}

float SAFEAudioProcessor::getGainParameter (int index)
{
    return parameters [index]->getGainValue();
}

const String SAFEAudioProcessor::getParameterName (int index)
{
    return parameters [index]->getName();
}

const String SAFEAudioProcessor::getParameterText (int index)
{
    SAFEParameter* info = parameters [index];
    return String (info->getUIScaledValue(), 2) + info->getUnits();
}

const OwnedArray <SAFEParameter>& SAFEAudioProcessor::getParameterArray()
{
    return parameters;
}

//==========================================================================
//      Other Plugin Info
//==========================================================================
const String SAFEAudioProcessor::getInputChannelName (int channelIndex) const
{
    return String (channelIndex + 1);
}

const String SAFEAudioProcessor::getOutputChannelName (int channelIndex) const
{
    return String (channelIndex + 1);
}

bool SAFEAudioProcessor::isInputChannelStereoPair (int /*index*/) const
{
    return true;
}

bool SAFEAudioProcessor::isOutputChannelStereoPair (int /*index*/) const
{
    return true;
}

bool SAFEAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool SAFEAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool SAFEAudioProcessor::silenceInProducesSilenceOut() const
{
    return false;
}

double SAFEAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==========================================================================
//      Program Stuff
//==========================================================================
int SAFEAudioProcessor::getNumPrograms()
{
    return 1;
}

int SAFEAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SAFEAudioProcessor::setCurrentProgram (int /*index*/)
{
}

const String SAFEAudioProcessor::getProgramName (int /*index*/)
{
    return String::empty;
}

void SAFEAudioProcessor::changeProgramName (int /*index*/, const String& /*newName*/)
{
}

//==========================================================================
//      Saving and Loading Patches
//==========================================================================
void SAFEAudioProcessor::getStateInformation (MemoryBlock& destData)
{
    XmlElement xml (makeXmlString(JucePlugin_Name + String ("Settings")));

    for (int parameterNum = 0; parameterNum < parameters.size(); ++parameterNum)
    {
        xml.setAttribute ("Parameter" + String (parameterNum), parameters [parameterNum]->getBaseValue());
    }

    copyXmlToBinary (xml, destData);
}

void SAFEAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    ScopedPointer<XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr)
    {
        if (xmlState->hasTagName (makeXmlString (JucePlugin_Name + String ("Settings"))))
        {
            for (int parameterNum = 0; parameterNum < parameters.size(); ++parameterNum)
            {
                setParameterNotifyingHost (parameterNum, (float) xmlState->getDoubleAttribute ("Parameter" + String (parameterNum), parameters [parameterNum]->getBaseValue()));
            }
        }
    }
}

//==========================================================================
//      Semantic Data Parsing
//==========================================================================
void SAFEAudioProcessor::initialiseSemanticDataFile()
{
    File documentsDirectory (File::getSpecialLocation (File::userDocumentsDirectory));

    File dataDirectory (documentsDirectory.getChildFile ("SAFEPluginData"));

    if (! dataDirectory.isDirectory())
    {
        dataDirectory.createDirectory();
    }

    semanticDataFile = dataDirectory.getChildFile (JucePlugin_Name + String ("Data.xml"));

    if (semanticDataFile.exists())
    {
        XmlDocument semanticDataDocument (semanticDataFile);
        semanticDataElement = semanticDataDocument.getDocumentElement();
    }
    else
    {
        String elementName (JucePlugin_Name + String ("Data"));
        semanticDataElement = new XmlElement (makeXmlString (elementName));
        semanticDataElement->writeToFile (semanticDataFile, "");
    }
}

XmlElement* SAFEAudioProcessor::getSemanticDataElement()
{
    updateSemanticDataElement();

    return semanticDataElement.get();
}

WarningID SAFEAudioProcessor::populateXmlElementWithSemanticData (XmlElement* element, const SAFEMetaData& metaData)
{
    // analyse the buffered samples
    WarningID warning = analyseRecordedSamples();

    if (warning != NoWarning)
    {
        return warning;
    }

    // save the channel configuration
    XmlElement* configElement = element->createNewChildElement ("PlugInConfiguration");

    configElement->setAttribute ("PluginCode", getPluginCode());
    configElement->setAttribute ("Inputs", numInputs);
    configElement->setAttribute ("Outputs", numOutputs);
    configElement->setAttribute ("SampleRate", fs);
    configElement->setAttribute ("AnalysisTime", getAnalysisTime());

    // save the parameter settings
    XmlElement* parametersElement = element->createNewChildElement ("ParameterSettings");
    String parameterString;

    for (int parameterNum = 0; parameterNum < parameters.size() - 1; ++parameterNum)
    {
        float currentParameterValue = parametersToSave [parameterNum];
        parameterString += String (currentParameterValue) + ", ";
    }

    if (parameters.size() > 0)
    {
        parameterString += String (parametersToSave.getLast());
    }

    parametersElement->setAttribute ("Values", parameterString);

    // save the unprocessed audio features
    XmlElement* unprocessedFeaturesElement = element->createNewChildElement ("UnprocessedAudioFeatures");
    unprocessedFeatureExtractor.addFeaturesToXmlElement (unprocessedFeaturesElement);

    // save the processed audio features
    XmlElement* processedFeaturesElement = element->createNewChildElement ("ProcessedAudioFeatures");
    processedFeatureExtractor.addFeaturesToXmlElement (processedFeaturesElement);

    // save the meta data
    XmlElement* metaDataElement = element->createNewChildElement ("MetaData");

    metaDataElement->setAttribute ("Genre", metaData.genre);
    metaDataElement->setAttribute ("Instrument", metaData.instrument);
    metaDataElement->setAttribute ("Location", metaData.location);
    metaDataElement->setAttribute ("Experience", metaData.experience);
    metaDataElement->setAttribute ("Age", metaData.age);
    metaDataElement->setAttribute ("Language", metaData.language);

    return warning;
}

WarningID SAFEAudioProcessor::saveSemanticData (const String& newDescriptors, const SAFEMetaData& metaData)
{
}

WarningID SAFEAudioProcessor::loadSemanticData (const String& descriptor)
{
    StringArray descriptorArray;
    descriptorArray.addTokens (descriptor, " ,;", String::empty);

    updateSemanticDataElement();

    if (descriptorArray.size() == 0)
    {
        return DescriptorNotInFile;
    }

    String firstDescriptor = descriptorArray [0];

    // go through XML elements and look for the first one with the descriptor we want
    forEachXmlChildElement (*semanticDataElement, descriptorElement)
    {
        int numAttributes = descriptorElement->getNumAttributes();

        for (int attribute = 0; attribute < numAttributes; ++ attribute)
        {
            String attributeValue = descriptorElement->getAttributeValue (attribute);

            if (attributeValue == firstDescriptor)
            {
                XmlElement *parameterElement = descriptorElement->getChildByName ("ParameterSettings");
                String parameterValueString = parameterElement->getStringAttribute ("Values");

                StringArray parameterValues;
                parameterValues.addTokens (parameterValueString, ",", "");
                parameterValues.removeEmptyStrings();

                for (int i = 0; i < parameterValues.size(); ++i)
                {
                    setScaledParameterNotifyingHost (i, parameterValues [i].getDoubleValue());
                }
            }
        }
    }

    return NoWarning;
}

WarningID SAFEAudioProcessor::sendDataToServer (const String& newDescriptors, const SAFEMetaData& metaData)
{
    // run the analysis
    WarningID warning = analyseRecordedSamples();

    // rdf object
    LibrdfHolder rdf;
    
    // create a node for the plug-in
    String implementationName = getPluginImplementationString();
    LibrdfHolder::NodePointer pluginNode (librdf_new_node_from_uri_local_name (rdf.world.get(),
                                                                               rdf.afxdb.get(),
                                                                               (const unsigned char*) implementationName.toRawUTF8()),
                                          librdf_free_node);

    // create a node for the transform
    LibrdfHolder::NodePointer transformNode (librdf_new_node_from_uri_local_name (rdf.world.get(),
                                                                                  rdf.safedb.get(),
                                                                                  (const unsigned char*) "transform_@uniqueID"),
                                             librdf_free_node);

    // details about the transform
    rdf.addTriple (transformNode, rdf.rdfType, rdf.provActivity);
    rdf.addTriple (transformNode, rdf.rdfType, rdf.studioTransform);
    rdf.addTriple (transformNode, rdf.provWasAssociatedWith, rdf.dummyUser);
    rdf.addTriple (transformNode, rdf.provWasAssociatedWith, pluginNode);
    rdf.addTriple (transformNode, rdf.studioEffect, pluginNode);

    // location metadata
    LibrdfHolder::NodePointer locationNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                                 (const unsigned char*) "location_@uniqueID"), 
                                            librdf_free_node);
    rdf.addTriple (transformNode, rdf.safeMetadata, locationNode);
    rdf.addTriple (locationNode, rdf.rdfType, rdf.safeMetadataItem);
    rdf.addTriple (locationNode, rdf.rdfsLabel, "location");
    rdf.addTriple (locationNode, rdf.rdfsComment, metaData.location);

    LibrdfHolder::NodePointer locationActivityNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (locationNode, rdf.provWasGeneratedBy, locationActivityNode);
    rdf.addTriple (locationActivityNode, rdf.rdfType, rdf.provActivity);
    rdf.addTriple (locationActivityNode, rdf.provWasAssociatedWith, rdf.dummyUser);

    // instrument metadata
    LibrdfHolder::NodePointer instrumentNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                                   (const unsigned char*) "instrument_@uniqueID"), 
                                              librdf_free_node);
    rdf.addTriple (instrumentNode, rdf.rdfType, rdf.safeMetadataItem);
    rdf.addTriple (instrumentNode, rdf.rdfsLabel, "instrument");
    rdf.addTriple (instrumentNode, rdf.rdfsComment, metaData.instrument);

    LibrdfHolder::NodePointer instrumentActivityNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (instrumentNode, rdf.provWasGeneratedBy, instrumentActivityNode);
    rdf.addTriple (instrumentActivityNode, rdf.rdfType, rdf.provActivity);
    rdf.addTriple (instrumentActivityNode, rdf.provWasAssociatedWith, rdf.dummyUser);

    // genre metadata
    LibrdfHolder::NodePointer genreNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                              (const unsigned char*) "genre_@uniqueID"), 
                                         librdf_free_node);
    rdf.addTriple (genreNode, rdf.rdfType, rdf.safeMetadataItem);
    rdf.addTriple (genreNode, rdf.rdfsLabel, "genre");
    rdf.addTriple (genreNode, rdf.rdfsComment, metaData.genre);

    LibrdfHolder::NodePointer genreActivityNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (genreNode, rdf.provWasGeneratedBy, genreActivityNode);
    rdf.addTriple (genreActivityNode, rdf.rdfType, rdf.provActivity);
    rdf.addTriple (genreActivityNode, rdf.provWasAssociatedWith, rdf.dummyUser);

    // descriptors
    LibrdfHolder::NodePointer descriptorNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (transformNode, rdf.safeDescriptor, descriptorNode);
    rdf.addTriple (descriptorNode, rdf.rdfType, rdf.safeDescriptorItem);
    rdf.addTriple (descriptorNode, rdf.rdfsComment, newDescriptors);

    LibrdfHolder::NodePointer descriptorActivityNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (descriptorNode, rdf.provWasGeneratedBy, descriptorActivityNode);
    rdf.addTriple (descriptorActivityNode, rdf.rdfType, rdf.provActivity);
    rdf.addTriple (descriptorActivityNode, rdf.provWasAssociatedWith, rdf.dummyUser);

    // plugin state
    LibrdfHolder::NodePointer stateNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                              (const unsigned char*) "exState_@uniqueID"), librdf_free_node);
    rdf.addTriple (transformNode, rdf.afxState, stateNode);

    // get parameter settings
    for (int i = 0; i < parameters.size(); ++i)
    {
        // make some nodes for it (too many I feel)
        String parameterString  = "par" + String (i) + "Value_@uniqueID";
        LibrdfHolder::NodePointer parameterSettingNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL),
                                                        librdf_free_node);
        LibrdfHolder::NodePointer parameterNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
        LibrdfHolder::NodePointer parameterIdNode (librdf_new_node_from_typed_literal (rdf.world.get(),
                                                            (const unsigned char*) String (i).toRawUTF8(),
                                                             NULL,
                                                             rdf.xsdInteger.get()),
                                                   librdf_free_node);
        LibrdfHolder::NodePointer parameterValueNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                        (const unsigned char*) parameterString.toRawUTF8()), 
                                                      librdf_free_node);
        String parameterValue = String (parameters [i]->getScaledValue());
        LibrdfHolder::NodePointer parameterLiteralNode (librdf_new_node_from_typed_literal (rdf.world.get(),
                                                                 (const unsigned char*) parameterValue.toRawUTF8(),
                                                                 NULL,
                                                                 rdf.xsdDouble.get()),
                                                        librdf_free_node);

        // link those nodes together
        rdf.addTriple (stateNode, rdf.afxParameterSetting, parameterSettingNode);
        rdf.addTriple (parameterSettingNode, rdf.rdfType, rdf.afxParameterSettingItem);
        rdf.addTriple (parameterSettingNode, rdf.afxParameter, parameterNode);
        rdf.addTriple (parameterNode, rdf.afxParameterId, parameterIdNode);
        rdf.addTriple (parameterNode, rdf.qudtValue, parameterValueNode);
        rdf.addTriple (parameterValueNode, rdf.qudtNumericValue, parameterLiteralNode);
    }

    // associations
    LibrdfHolder::NodePointer pluginAssociationNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                                          (const unsigned char*) "association_1_@uniqueID"),
                                                     librdf_free_node);
    LibrdfHolder::NodePointer pluginRoleNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (pluginAssociationNode, rdf.rdfType, rdf.provAssociation);
    rdf.addTriple (pluginAssociationNode, rdf.provAgent, pluginNode);
    rdf.addTriple (pluginAssociationNode, rdf.provQualifiedAssociation, transformNode);
    rdf.addTriple (pluginAssociationNode, rdf.provHadRole, pluginRoleNode);
    rdf.addTriple (pluginRoleNode, rdf.rdfsComment, "audio effect plug-in");

    LibrdfHolder::NodePointer userAssociationNode (librdf_new_node_from_uri_local_name (rdf.world.get(), rdf.safedb.get(),
                                                                                        (const unsigned char*) "association_2_@uniqueID"),
                                                   librdf_free_node);
    LibrdfHolder::NodePointer userRoleNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);
    rdf.addTriple (userAssociationNode, rdf.rdfType, rdf.provAssociation);
    rdf.addTriple (userAssociationNode, rdf.provAgent, rdf.dummyUser);
    rdf.addTriple (userAssociationNode, rdf.provQualifiedAssociation, transformNode);
    rdf.addTriple (userAssociationNode, rdf.provHadRole, userRoleNode);
    rdf.addTriple (userRoleNode, rdf.rdfsComment, "configure/apply effect plug-in");

    // signal and timeline nodes
    // inputs
    OwnedArray <LibrdfHolder::NodePointer> inputSignalNodes;
    OwnedArray <LibrdfHolder::NodePointer> inputTimelineNodes;

    for (int i = 0; i < numInputs; ++i)
    {
        String signalName = "input_signal_" + String (i) + "_@uniqueID";
        String timelineName = "input_signal_timeline_" + String (i) + "_@uniqueID";
        String signalString = "input channel " + String (i);

        LibrdfHolder::NodePointer *signalNode = 
          inputSignalNodes.add (new LibrdfHolder::NodePointer (librdf_new_node_from_uri_local_name (rdf.world.get(), 
                                                                                  rdf.safedb.get(),
                                                                                  (const unsigned char*) signalName.toRawUTF8()),
                                                               librdf_free_node));
        LibrdfHolder::NodePointer *timelineNode = 
          inputTimelineNodes.add (new LibrdfHolder::NodePointer (librdf_new_node_from_uri_local_name (rdf.world.get(), 
                                                                                    rdf.safedb.get(),
                                                                                    (const unsigned char*) timelineName.toRawUTF8()),
                                                                 librdf_free_node));
        LibrdfHolder::NodePointer intervalNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);

        rdf.addTriple (*signalNode, rdf.rdfType, rdf.moSignal);
        rdf.addTriple (*signalNode, rdf.rdfsLabel, signalString);
        rdf.addTriple (*signalNode, rdf.moTime, intervalNode);
        rdf.addTriple (intervalNode, rdf.rdfType, rdf.tlInterval);
        rdf.addTriple (intervalNode, rdf.tlOnTimeline, *timelineNode);
        rdf.addTriple (*timelineNode, rdf.rdfType, rdf.tlTimeline);
        rdf.addTriple (transformNode, rdf.provUsed, *signalNode);
        rdf.addTriple (*signalNode, rdf.safeMetadata, instrumentNode);
        rdf.addTriple (*signalNode, rdf.safeMetadata, genreNode);
    }

    // outputs
    OwnedArray <LibrdfHolder::NodePointer> outputSignalNodes;
    OwnedArray <LibrdfHolder::NodePointer> outputTimelineNodes;

    for (int i = 0; i < numOutputs; ++i)
    {
        String signalName = "output_signal_" + String (i) + "_@uniqueID";
        String timelineName = "output_signal_timeline_" + String (i) + "_@uniqueID";
        String signalString = "output channel " + String (i);

        LibrdfHolder::NodePointer *signalNode = 
          outputSignalNodes.add (new LibrdfHolder::NodePointer (librdf_new_node_from_uri_local_name (rdf.world.get(), 
                                                                                  rdf.safedb.get(),
                                                                                  (const unsigned char*) signalName.toRawUTF8()),
                                                                librdf_free_node));
        LibrdfHolder::NodePointer *timelineNode = 
          outputTimelineNodes.add (new LibrdfHolder::NodePointer (librdf_new_node_from_uri_local_name (rdf.world.get(), 
                                                                                    rdf.safedb.get(),
                                                                                    (const unsigned char*) timelineName.toRawUTF8()),
                                                                  librdf_free_node));
        LibrdfHolder::NodePointer intervalNode (librdf_new_node_from_blank_identifier (rdf.world.get(), NULL), librdf_free_node);

        rdf.addTriple (*signalNode, rdf.rdfType, rdf.moSignal);
        rdf.addTriple (*signalNode, rdf.rdfsLabel, signalString);
        rdf.addTriple (*signalNode, rdf.moTime, intervalNode);
        rdf.addTriple (intervalNode, rdf.rdfType, rdf.tlInterval);
        rdf.addTriple (intervalNode, rdf.tlOnTimeline, *timelineNode);
        rdf.addTriple (*timelineNode, rdf.rdfType, rdf.tlTimeline);
        rdf.addTriple (transformNode, rdf.provGenerated, *signalNode);
        rdf.addTriple (*signalNode, rdf.safeMetadata, instrumentNode);
        rdf.addTriple (*signalNode, rdf.safeMetadata, genreNode);
    }

    // add the feature values
    unprocessedFeatureExtractor.addFeaturesToRdf (rdf, inputSignalNodes, inputTimelineNodes);
    processedFeatureExtractor.addFeaturesToRdf (rdf, outputSignalNodes, outputTimelineNodes);

    // save it to a file
    File documentsDirectory (File::getSpecialLocation (File::userDocumentsDirectory));
    File dataDirectory (documentsDirectory.getChildFile ("SAFEPluginData"));
    File tempRdfFile = dataDirectory.getChildFile ("Temp.ttl");

    FILE *rdfFile;
    rdfFile = fopen (tempRdfFile.getFullPathName().toRawUTF8(), "w");
    librdf_serializer_serialize_model_to_file_handle (rdf.serializer.get(), rdfFile, NULL, rdf.model.get());
    fclose (rdfFile);

    // zip it up for sending to the server
    File zipFile = dataDirectory.getChildFile ("SemanticData.zip");

    {
        FileOutputStream zipStream (zipFile);
        ZipFile::Builder zipper;
        zipper.addFile (tempRdfFile, 9);
        double progress = 0;
        zipper.writeToStream (zipStream, &progress);
        
        while (progress < 1.0)
        {
            Thread::sleep (100);
        }
    }

    #if JUCE_LINUX
    CURLcode res;
 
    struct curl_httppost *formpost=NULL;
    struct curl_httppost *lastptr=NULL;
    struct curl_slist *headerlist=NULL;
    static const char buf[] = "Expect:";
 
    /* Fill in the file upload field */ 
    curl_formadd(&formpost,
                 &lastptr,
                 CURLFORM_COPYNAME, "turtles",
                 CURLFORM_FILE, zipFile.getFullPathName().toRawUTF8(),
                 CURLFORM_END);
 
    /* Fill in the filename field */ 
    curl_formadd(&formpost,
                 &lastptr,
                 CURLFORM_COPYNAME, "turtles",
                 CURLFORM_COPYCONTENTS, zipFile.getFullPathName().toRawUTF8(),
                 CURLFORM_END);
 
 
    /* Fill in the submit field too, even if this is rarely needed */ 
    curl_formadd(&formpost,
                 &lastptr,
                 CURLFORM_COPYNAME, "submit",
                 CURLFORM_COPYCONTENTS, "send",
                 CURLFORM_END);
 
    headerlist = curl_slist_append(headerlist, buf);
    if(curl) 
    {
        curl_easy_setopt(curl->curl, CURLOPT_URL, "http://193.60.133.151/pokemon");
        curl_easy_setopt(curl->curl, CURLOPT_HTTPPOST, formpost);
 
        res = curl_easy_perform(curl->curl);
 
        curl_formfree(formpost);
        curl_slist_free_all (headerlist);
    }
    #else
    URL dataUpload ("http://193.60.133.151/pokemon/index.php");
    dataUpload = dataUpload.withFileToUpload ("turtles", zipFile, "");

    ScopedPointer <InputStream> stream (dataUpload.createInputStream (true));
    #endif

    tempRdfFile.deleteFile();
    zipFile.deleteFile();

    return warning;
}

WarningID SAFEAudioProcessor::getServerData (const String& descriptor)
{
    // get descriptors
    StringArray descriptorArray;
    descriptorArray.addTokens (descriptor, " ,;", String::empty);
    descriptorArray.removeEmptyStrings();

    if (descriptorArray.size() > 0)
    {
        // we just take the first descriptor in the box
        // This will be changed to a more comprehensive model soon...
        String firstDescriptor = descriptorArray [0];
        
        // send call to script with user descriptor...
        URL downloadParamData ("http://193.60.133.151/newsafe/getaverageparameters.php");
        downloadParamData = downloadParamData.withParameter("Plugin", getPluginCode());
        downloadParamData = downloadParamData.withParameter("Descriptor", firstDescriptor);
        
        // just returns an ordered list of strings for the params...
        String dbOutput = downloadParamData.readEntireTextStream();

        if (dbOutput == "Descriptor not found.")
        {
            return DescriptorNotOnServer;
        }

        StringArray parameterSettings;
        parameterSettings.addTokens (dbOutput, "\n", "");
        parameterSettings.removeEmptyStrings();

        for (int i = 0; i < parameterSettings.size(); ++i)
        {
            double parameterValue = parameterSettings [i].fromFirstOccurrenceOf (", ", false, false).getDoubleValue();

            setScaledParameterNotifyingHost (i, parameterValue);                        
        }
    }
    else
    {
        return DescriptorNotOnServer;
    }

	return NoWarning;
}

//==========================================================================
//      Analysis Thread
//==========================================================================
WarningID SAFEAudioProcessor::startAnalysisThread()
{
    if (analysisThread->isThreadRunning())
    {
        resetRecording();
        sendWarningToEditor (AnalysisThreadBusy);
        return AnalysisThreadBusy;
    }
    else
    {
        resetRecording();
        analysisThread->setParameters (descriptorsToSave, metaDataToSave, sendToServer);
        analysisThread->startThread();
    }

    return NoWarning;
}

bool SAFEAudioProcessor::isThreadRunning()
{
    return analysisThread->isThreadRunning();
}

void SAFEAudioProcessor::sendWarningToEditor (WarningID warning)
{
    SAFEAudioProcessorEditor* editor = static_cast <SAFEAudioProcessorEditor*> (getActiveEditor());

    if (editor)
    {
        editor->flagWarning (warning);
    }
}

//==========================================================================
//      Generate a details XML
//==========================================================================
void SAFEAudioProcessor::saveDetailsToXml()
{
    XmlElement parentElement ("Plugin");
    parentElement.setAttribute ("Name", JucePlugin_Name);
    parentElement.setAttribute ("Code", getPluginCode());

    String parameterString;

    for (int i = 0; i < parameters.size() - 1; ++i)
    {
        parameterString += parameters [i]->getName() + ", ";
    }

    if (parameters.size() > 0)
    {
        parameterString += parameters.getLast()->getName();
    }

    parentElement.setAttribute ("Parameters", parameterString);

    File documentsDirectory (File::getSpecialLocation (File::userDocumentsDirectory));
    File dataDirectory (documentsDirectory.getChildFile ("SAFEPluginData"));

    File tempDataFile = dataDirectory.getChildFile (JucePlugin_Name + String ("Details.xml"));

    parentElement.writeToFile (tempDataFile, "");
}

//==========================================================================
//      Generate a details RDF
//==========================================================================
void SAFEAudioProcessor::saveDetailsToRdf()
{
    File documentsDirectory (File::getSpecialLocation (File::userDocumentsDirectory));
    File dataDirectory (documentsDirectory.getChildFile ("SAFEPluginData"));
    File tempDataFile = dataDirectory.getChildFile (JucePlugin_Name + String ("Details.ttl"));

    // some handy rdf stuff
    LibrdfHolder rdf;

    // get the plug-ins format and version
    String pluginFormat (getPluginFormat());
    String versionNumber (JucePlugin_VersionString);
    String implementationString = getPluginImplementationString();

    // create a node for the plug-in
    String implementationName = implementationString;
    LibrdfHolder::NodePointer pluginNode (librdf_new_node_from_uri_local_name (rdf.world.get(),
                                                                               rdf.afxdb.get(),
                                                                               (const unsigned char*) implementationName.toRawUTF8()),
                                          librdf_free_node);

    // plug-in is an audio effect implementation
    rdf.addTriple (pluginNode, rdf.rdfType, rdf.afxImplementation);

    // plug-in is a software agent
    rdf.addTriple (pluginNode, rdf.rdfType, rdf.provSoftwareAgent);

    // plug-ins parameters
    for (int i = 0; i < parameters.size(); ++i)
    {
        // create a blank node for the parameter
        String parameterNodeName = "param" + String (i);
        LibrdfHolder::NodePointer parameterNode (librdf_new_node_from_blank_identifier (rdf.world.get(),
                                                                   (const unsigned char*) parameterNodeName.toRawUTF8()),
                                                 librdf_free_node);

        // parameter belongs to plug-in
        rdf.addTriple (pluginNode, rdf.afxHasParameter, parameterNode);

        // parameter is a parameter
        rdf.addTriple (parameterNode, rdf.rdfType, rdf.afxNumParameter);

        // parameter name
        String parameterName = parameters [i]->getName();
        LibrdfHolder::NodePointer parameterNameNode (librdf_new_node_from_typed_literal (rdf.world.get(),
                                                                                (const unsigned char*) parameterName.toRawUTF8(),
                                                                                NULL,
                                                                                rdf.xsdString.get()),
                                                     librdf_free_node);
        rdf.addTriple (parameterNode, rdf.rdfsLabel, parameterNameNode);

        // parameter id
        LibrdfHolder::NodePointer parameterIdNode (librdf_new_node_from_typed_literal (rdf.world.get(),
                                                            (const unsigned char*) String (i).toRawUTF8(),
                                                             NULL,
                                                             rdf.xsdInteger.get()),
                                                   librdf_free_node);
        rdf.addTriple (parameterNode, rdf.afxParameterId, parameterIdNode);

        // default value
        String defaultValueName = parameterNodeName + "default";
        LibrdfHolder::NodePointer defaultValueNode (librdf_new_node_from_blank_identifier (rdf.world.get(),
                                                                      (const unsigned char*) defaultValueName.toRawUTF8()),
                                                    librdf_free_node);
        rdf.addTriple (parameterNode, rdf.afxDefaultValue, defaultValueNode);
        rdf.addTriple (defaultValueNode, rdf.rdfType, rdf.qudtQuantityValue);
        rdf.addTriple (defaultValueNode, rdf.qudtNumericValue, String (parameters [i]->getDefaultValue()));

        // minimum value
        String minValueName = parameterNodeName + "min";
        LibrdfHolder::NodePointer minValueNode (librdf_new_node_from_blank_identifier (rdf.world.get(),
                                                                  (const unsigned char*) minValueName.toRawUTF8()),
                                                    librdf_free_node);
        rdf.addTriple (parameterNode, rdf.afxMinValue, minValueNode);
        rdf.addTriple (minValueNode, rdf.rdfType, rdf.qudtQuantityValue);
        rdf.addTriple (minValueNode, rdf.qudtNumericValue, String (parameters [i]->getMinValue()));
        
        // maximum value
        String maxValueName = parameterNodeName + "max";
        LibrdfHolder::NodePointer maxValueNode (librdf_new_node_from_blank_identifier (rdf.world.get(),
                                                                  (const unsigned char*) maxValueName.toRawUTF8()),
                                                    librdf_free_node);
        rdf.addTriple (parameterNode, rdf.afxMaxValue, maxValueNode);
        rdf.addTriple (maxValueNode, rdf.rdfType, rdf.qudtQuantityValue);
        rdf.addTriple (maxValueNode, rdf.qudtNumericValue, String (parameters [i]->getMaxValue()));
    }

    FILE *rdfFile;
    rdfFile = fopen (tempDataFile.getFullPathName().toRawUTF8(), "w");
    librdf_serializer_serialize_model_to_file_handle (rdf.serializer.get(), rdfFile, NULL, rdf.model.get());
    fclose (rdfFile);
}

//==========================================================================
//      Process Block
//==========================================================================
void SAFEAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // get the channel configuration
    numInputs = getNumInputChannels();
    numOutputs = getNumOutputChannels();
    fs = sampleRate;

    // work out how many frames we will get in the analysis time 
    int samplesInRecording = (int) floor (sampleRate * getAnalysisTime() / 1000);
    numAnalysisFrames = (int) floor ((float) (samplesInRecording / getAnalysisFrameSize()));
    numSamplesToRecord = numAnalysisFrames * getAnalysisFrameSize();

    // set up analysis buffers
    unprocessedBuffer.setSize (numInputs, numSamplesToRecord);
    processedBuffer.setSize (numOutputs, numSamplesToRecord);

    unprocessedFeatureExtractor.initialise (numInputs, getAnalysisFrameSize(), getAnalysisStepSize(), sampleRate);
    processedFeatureExtractor.initialise (numOutputs, getAnalysisFrameSize(), getAnalysisStepSize(), sampleRate);

    for (int i = 0; i < parameters.size(); ++i)
    {
        parameters [i]->setSampleRate (sampleRate);
    }

    controlBlockSize = (int) (sampleRate / controlRate);
    midiControlBlock.ensureSize (2048);
    midiControlBlock.clear();
    
    // call any prep the plugin processing wants to do
    pluginPreparation (sampleRate, samplesPerBlock);
}

void SAFEAudioProcessor::processBlock (AudioSampleBuffer& buffer, MidiBuffer& midiMessages)
{
    localRecording = recording;

    recordUnprocessedSamples (buffer);

    // call the plugin dsp
    bool parametersInterpolating = false;

    for (int i = 0; i < parameters.size(); ++i)
    {
        parametersInterpolating = parametersInterpolating || parameters [i]->isInterpolating();
    }

    if (parametersInterpolating)
    {
        int numChannels = buffer.getNumChannels();
        int numSamples = buffer.getNumSamples();

        if (numSamples < remainingControlBlockSamples)
        {
            AudioSampleBuffer controlBlock (buffer.getArrayOfWritePointers(), numChannels, numSamples);

            midiControlBlock.clear();
            midiControlBlock.addEvents (midiMessages, 0, numSamples, 0);

            pluginProcessing (controlBlock, midiControlBlock);

            remainingControlBlockSamples -= numSamples;
        }
        else
        {
            if (remainingControlBlockSamples)
            {
                AudioSampleBuffer controlBlock (buffer.getArrayOfWritePointers(), numChannels, remainingControlBlockSamples);

                midiControlBlock.clear();
                midiControlBlock.addEvents (midiMessages, 0, remainingControlBlockSamples, 0);

                pluginProcessing (controlBlock, midiControlBlock);
            }
        
            int numControlBlocks = (int) ((numSamples - remainingControlBlockSamples) / controlBlockSize);
            int sampleNumber = remainingControlBlockSamples;

            for (int block = 0; block < numControlBlocks; ++block)
            {
                for (int i = 0; i < parameters.size(); ++i)
                {
                    if (parameters [i]->isInterpolating())
                    {
                        parameters [i]->smoothValues();
                        parameterUpdateCalculations (i);
                    }
                }

                AudioSampleBuffer controlBlock (buffer.getArrayOfWritePointers(), numChannels, sampleNumber, controlBlockSize);

                midiControlBlock.clear();
                midiControlBlock.addEvents (midiMessages, sampleNumber, controlBlockSize, 0);

                pluginProcessing (controlBlock, midiControlBlock);

                sampleNumber += controlBlockSize;
            }

            int samplesLeft = numSamples - sampleNumber;

            if (samplesLeft)
            {
                for (int i = 0; i < parameters.size(); ++i)
                {
                    if (parameters [i]->isInterpolating())
                    {
                        parameters [i]->smoothValues();
                        parameterUpdateCalculations (i);
                    }
                }

                AudioSampleBuffer controlBlock (buffer.getArrayOfWritePointers(), numChannels, sampleNumber, samplesLeft);

                midiControlBlock.clear();
                midiControlBlock.addEvents (midiMessages, sampleNumber, samplesLeft, 0);

                pluginProcessing (controlBlock, midiControlBlock);
            }

            remainingControlBlockSamples = controlBlockSize - samplesLeft;
        }
    }
    else
    {
        pluginProcessing (buffer, midiMessages);
        remainingControlBlockSamples = 0;
    }

    // In case we have more outputs than inputs, we'll clear any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    for (int i = getNumInputChannels(); i < getNumOutputChannels(); ++i)
    {
        buffer.clear (i, 0, buffer.getNumSamples());
    }

    updatePlayHead();
    recordProcessedSamples (buffer);
}

//==========================================================================
//      Playing & Recording Info
//==========================================================================
bool SAFEAudioProcessor::isPlaying()
{
    return playHead.isPlaying;
}

bool SAFEAudioProcessor::startRecording (const String& descriptors, const SAFEMetaData& metaData, bool newSendToServer)
{
    if (readyToSave)
    {
        currentUnprocessedAnalysisFrame = 0;
        currentProcessedAnalysisFrame = 0;
        unprocessedTap = 0;
        processedTap = 0;
        unprocessedSamplesToRecord = numSamplesToRecord;
        processedSamplesToRecord = numSamplesToRecord;

        descriptorsToSave = descriptors;
        metaDataToSave = metaData;
        sendToServer = newSendToServer;
        cacheCurrentParameters();

        recording = true;
        readyToSave = false;

        startTimer (50);
        return true;
    }
    else
    {
        return false;
    }
}

bool SAFEAudioProcessor::isRecording()
{
    return recording;
}

bool SAFEAudioProcessor::isReadyToSave()
{
    return readyToSave;
}

int SAFEAudioProcessor::getAnalysisFrameSize()
{
    return 4096;
}

int SAFEAudioProcessor::getAnalysisStepSize()
{
    return 4096;
}

int SAFEAudioProcessor::getAnalysisTime()
{
    return 5000;
}

void SAFEAudioProcessor::setSpectralAnalysisWindowingFunction (void (*newWindowingFunction) (float* audioData, int numSamples))
{
    unprocessedFeatureExtractor.setWindowingFunction (newWindowingFunction);
    processedFeatureExtractor.setWindowingFunction (newWindowingFunction);
}

//==========================================================================
//      Methods to Create New Parameters
//==========================================================================
void SAFEAudioProcessor::addParameter (String name, float& valueRef, float initialValue, float minValue, float maxValue, String units, float skewFactor, bool convertDBToGainValue, double interpolationTime, float UIScaleFactor)
{
    parameters.add (new SAFEParameter (name, valueRef, initialValue, minValue, maxValue, units, skewFactor, convertDBToGainValue, interpolationTime, UIScaleFactor));

    parametersToSave.add (0);
}

void SAFEAudioProcessor::addDBParameter (String name, float& valueRef, float initialValue, float minValue, float maxValue, String units, float skewFactor, double interpolationTime, float UIScaleFactor)
{
    parameters.add (new SAFEParameter (name, valueRef, initialValue, minValue, maxValue, units, skewFactor, true, interpolationTime, UIScaleFactor));

    parametersToSave.add (0);
}

//==========================================================================
//      Add Features to Extract
//==========================================================================
void SAFEAudioProcessor::addLibXtractFeature (LibXtract::Feature feature)
{
    unprocessedFeatureExtractor.addLibXtractFeature (feature);
    processedFeatureExtractor.addLibXtractFeature (feature);
}

void SAFEAudioProcessor::addVampPlugin (const String &libraryName, const String &pluginName)
{
    unprocessedFeatureExtractor.addVampPlugin (libraryName, pluginName);
    processedFeatureExtractor.addVampPlugin (libraryName, pluginName);
}

//==========================================================================
//      Buffer Playing Audio For Analysis
//==========================================================================
void SAFEAudioProcessor::recordUnprocessedSamples (AudioSampleBuffer& buffer)
{
    if (localRecording)
    {
        int numSamples = buffer.getNumSamples();

        if (unprocessedSamplesToRecord < numSamples)
        {
            numSamples = unprocessedSamplesToRecord;
        }

        for (int channel = 0; channel < numOutputs; ++channel)
        {
            unprocessedBuffer.copyFrom (channel, unprocessedTap, buffer, channel, 0, numSamples);
        }

        unprocessedTap += numSamples;
        unprocessedSamplesToRecord -= numSamples;
    }
}

void SAFEAudioProcessor::recordProcessedSamples (AudioSampleBuffer& buffer)
{
    if (localRecording)
    {
        int numSamples = buffer.getNumSamples();

        if (processedSamplesToRecord < numSamples)
        {
            numSamples = processedSamplesToRecord;
        }

        for (int channel = 0; channel < numOutputs; ++channel)
        {
            processedBuffer.copyFrom (channel, processedTap, buffer, channel, 0, numSamples);
        }

        processedTap += numSamples;
        processedSamplesToRecord -= numSamples;

        if (processedSamplesToRecord == 0)
        {
            startAnalysisThread();
        }
    }
}

//==========================================================================
//      Buffer Playing Audio For Analysis
//==========================================================================
WarningID SAFEAudioProcessor::analyseRecordedSamples()
{
    unprocessedFeatureExtractor.analyseAudio (unprocessedBuffer);
    processedFeatureExtractor.analyseAudio (processedBuffer);

    return NoWarning;
}

//==========================================================================
//      Play Head Stuff
//==========================================================================
void SAFEAudioProcessor::updatePlayHead()
{
    AudioPlayHead::CurrentPositionInfo newPlayHead;
    
    if (getPlayHead() != nullptr && getPlayHead()->getCurrentPosition (newPlayHead))
    {
        playHead = newPlayHead;
    }
    else
    {
        playHead.resetToDefault();
    }
}

//==========================================================================
//      Semantic Data File Stuff
//==========================================================================
void SAFEAudioProcessor::updateSemanticDataElement()
{
    XmlDocument semanticDataDocument (semanticDataFile);
    semanticDataElement = semanticDataDocument.getDocumentElement();
}

//==========================================================================
//      Recording Tests
//==========================================================================
void SAFEAudioProcessor::cacheCurrentParameters()
{
    for (int i = 0; i < parameters.size(); ++i)
    {
        parametersToSave.set (i, parameters [i]->getScaledValue());
    }
}

bool SAFEAudioProcessor::haveParametersChanged()
{
    bool returnValue = false;

    for (int i = 0; i < parameters.size(); ++i)
    {
        if (parametersToSave [i] != parameters [i]->getScaledValue())
        {
            returnValue = true;
            break;
        }
    }

    return returnValue;
}

void SAFEAudioProcessor::timerCallback()
{
    if (haveParametersChanged())
    {
        resetRecording();
        sendWarningToEditor (ParameterChange);
        readyToSave = true;
    }

    if (! isPlaying())
    {
        resetRecording();
        sendWarningToEditor (AudioNotPlaying);
        readyToSave = true;
    }
}

void SAFEAudioProcessor::resetRecording()
{
    recording = false;
    stopTimer();
}

//==========================================================================
//      Get plug-in type
//==========================================================================
String SAFEAudioProcessor::getPluginFormat()
{
    switch (wrapperType)
    {
        case wrapperType_Undefined:
            return "Undefined";
        case wrapperType_VST:
            return "VST";
        case wrapperType_VST3:
            return "VST3";
        case wrapperType_AudioUnit:
            return "AU";
        case wrapperType_RTAS:
            return "RTAS";
        case wrapperType_AAX:
            return "AAX";
        case wrapperType_Standalone:
            return "Standalone";
    }
}

String SAFEAudioProcessor::getPluginImplementationString()
{
    String pluginCode = getPluginCode();
    String pluginFormat (getPluginFormat());
    String versionNumber (JucePlugin_VersionString);
    String implementationName = "implementation_" + pluginCode + "_" + pluginFormat + "_" + versionNumber;
    return implementationName;
}

//==========================================================================
//      Make String ok for use in XML
//==========================================================================
String SAFEAudioProcessor::makeXmlString (String input)
{
    return input.retainCharacters ("1234567890qwertyuioplkjhgfdsazxcvbnmMNBVCXZASDFGHJKLPOIUYTREWQ:-_");
}
