
// MultipleCameraDlg.cpp : implementation file
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <functional> 
#include <cmath>  
#include <memory>
#include <cstdint>
#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/bind/bind.hpp>
#include "HikMultipleCameras.h"
#include "Container.h"


#ifdef DEBUG
#ifdef _MSC_VER 
#define DEBUG_PRINT(...) printf_s(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#endif
#else
#define DEBUG_PRINT(...) do {} while (0)
#endif

// FBS Calculator
thread_local unsigned count = 0;
thread_local double last = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
#define FPS_CALC(_WHAT_, ncurrCameraIndex) \
do \
{ \
    double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); \
    ++count; \
    if (now - last >= 1.0) \
    { \
      std::cerr << "\033[1;31m";\
      std::cerr << ncurrCameraIndex<< ". Camera,"<<" Average framerate("<< _WHAT_ << "): " << double(count)/double(now - last) << " fbs." <<  "\n"; \
      std::cerr << "\033[0m";\
      count = 0; \
      last = now; \
    } \
} while(false)

#define FPS_CALC_BUF(_WHAT_, buff) \
do \
{ \
    static unsigned count_buf = 0;\
    static double last_buf = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();\
    double now_buf = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); \
    ++count_buf; \
    if (now_buf - last_buf >= 1.0) \
    { \
      std::cerr << "\033[0m";\
      std::cerr << "Average framerate("<< _WHAT_ << "): " << double(count_buf)/double(now_buf - last_buf) << " Hz. Queue size: " << buff.getSize () << "\n"; \
      std::cerr << "\033[0m";\
      count_buf = 0; \
      last_buf = now_buf; \
    } \
}while(false)


// HikMultipleCameras dialog
HikMultipleCameras::HikMultipleCameras(ImageBuffer<std::vector<std::pair<MV_FRAME_OUT_INFO_EX, std::shared_ptr<uint8_t[]> >>> &buf, std::chrono::system_clock::time_point timePoint, const std::string& cameraSettingsFile):
      m_buf(buf)
    , m_nDeviceNum(0)
    , m_timePoint(timePoint)
    , m_bOpenDevice(false)
    , m_bStartGrabbing(false)
    , m_bStartConsuming(false)
    , m_entered(true)
    , m_nTriggerMode(MV_TRIGGER_MODE_OFF)
    , m_nDeviceKey(1)
    , n_nGroupKey(1)
    , m_nGroupMask(1)
    , m_sTriggerSource("")
    , m_sCameraSettingsFile(cameraSettingsFile)
    , m_nTriggerTimeInterval(0)
    , m_currentPairImagesInfo_Buff(nullptr)
   
{
	
    memset(&m_stDevList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    memset(&m_actionCMDInfo, 0, sizeof(MV_ACTION_CMD_INFO));
    memset(&m_actionCMDResList, 0, sizeof(MV_ACTION_CMD_RESULT_LIST));
   
   
    //EnumDevicesByIPAddress();
    EnumDevices();

    if (m_nDeviceNum > 0)
    {
       
        m_bImagesOk.resize(m_nDeviceNum, false);
        m_bImagesCheck.resize(m_nDeviceNum, false);
        m_bImagesReady.resize(m_nDeviceNum, true);
        m_params.resize(m_nDeviceNum, {0});
        m_stImagesInfo.resize(m_nDeviceNum, {0});
        m_nSaveImagesBufSize.resize(m_nDeviceNum, 0);
        m_its.resize(m_nDeviceNum, 0);
        m_pairImagesInfo_Buff.resize(m_nDeviceNum, std::make_pair(MV_FRAME_OUT_INFO_EX{0}, nullptr));
        m_mProduceMutexes = std::vector<std::mutex>(m_nDeviceNum);
        m_mProduceMutexes = std::vector<std::mutex>(m_nDeviceNum);
        m_cDataReadyCon = condVector(m_nDeviceNum);
        
         // if (!container.open((char*)"test_hikrobot_jpgs.mp4", true)){
        //     exit(0);
        // };


        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        std::chrono::seconds sec = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
        for (unsigned int i = 0 ; i < m_nDeviceNum ; i++)
        {
            memset(&(m_stImagesInfo[i]), 0, sizeof(MV_FRAME_OUT_INFO_EX));
            m_pDataForSaveImages.push_back(nullptr);
            m_pSaveImagesBuf.push_back(nullptr);
            m_pcMyCameras.push_back(std::make_unique<HikCamera>());
            // std::string fileNameTmp = "hikrobot_" + m_mapSerials[i] + "_" + std::to_string(sec.count()) + ".mp4";
            // if (!m_Containers[i].open((char*)fileNameTmp.c_str(), true))
                // exit(0);

        }
       

    

    } else 
    {
        printf("No device detected! Exiting...\n");
        exit(0);

    }
    
}


//  Set trigger mode
int HikMultipleCameras::SetTriggerMode(void)
{
    int nRet = -1;
    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            nRet = m_pcMyCameras[i]->SetEnumValue("TriggerMode", m_nTriggerMode);
            
            if (nRet != MV_OK)
            {
                printf("Set Trigger mode fail! DevIndex[%d], TriggerMode[%d], nRet[%#x]\r\n. Exiting...", i, m_nTriggerMode, nRet);
                return nRet;
            }
        }
    }
    return nRet;
}



// Thread Function for save images on disk for every camera
int HikMultipleCameras::ThreadConsumeFun(int nCurCameraIndex)
{
    if (m_pcMyCameras[nCurCameraIndex])
    {
        MV_SAVE_IMAGE_PARAM_EX stParam;
        memset(&stParam, 0, sizeof(MV_SAVE_IMAGE_PARAM_EX));
        uint64_t oldtimeStamp = 0;
        long long int oldmicroseconds = 0;
        while(m_bStartConsuming)
        {
            {
             
                std::unique_lock<std::mutex> lk(m_mProduceMutexes[nCurCameraIndex]);
                m_cDataReadyCon[nCurCameraIndex].wait(lk, [this, nCurCameraIndex] {
                    return m_bImagesOk[nCurCameraIndex];

                });

                if  (m_pSaveImagesBuf[nCurCameraIndex] == nullptr) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    printf("continue \n");
                    continue;
                }
                m_pDataForSaveImages[nCurCameraIndex].reset(new uint8_t[m_stImagesInfo[nCurCameraIndex].nWidth * m_stImagesInfo[nCurCameraIndex].nHeight * 4 + 2048]);

                if (m_pDataForSaveImages[nCurCameraIndex] == nullptr)
                {
                    break;
                }
           

                stParam.enImageType = MV_Image_Jpeg; 
                stParam.enPixelType =  m_stImagesInfo[nCurCameraIndex].enPixelType; 
                stParam.nWidth = m_stImagesInfo[nCurCameraIndex].nWidth;       
                stParam.nHeight = m_stImagesInfo[nCurCameraIndex].nHeight;       
                stParam.nDataLen = m_stImagesInfo[nCurCameraIndex].nFrameLen;
                stParam.pData = m_pSaveImagesBuf[nCurCameraIndex].get();
                stParam.pImageBuffer =  m_pDataForSaveImages[nCurCameraIndex].get();
                stParam.nBufferSize = m_stImagesInfo[nCurCameraIndex].nWidth * m_stImagesInfo[nCurCameraIndex].nHeight * 4 + 2048;;  
                stParam.nJpgQuality = 99;  
                
                m_bImagesOk[nCurCameraIndex] = false;
                
                int nRet =  m_pcMyCameras[nCurCameraIndex]->SaveImage(&stParam);

                if(nRet != MV_OK)
                {
                    printf("Failed in MV_CC_SaveImage,nRet[%x]\n", nRet);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;;
                }
                char filepath[256];
               
                std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
                std::chrono::microseconds ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
                long long int microseconds = ms.count();

                uint64_t timeStamp = (((uint64_t) m_stImagesInfo[nCurCameraIndex].nDevTimeStampHigh) << 32) + m_stImagesInfo[nCurCameraIndex].nDevTimeStampLow;

                uint64_t  timeDif = timeStamp - oldtimeStamp;
                uint64_t systemTimeDiff = microseconds - oldmicroseconds;
                oldtimeStamp = timeStamp; 
                oldmicroseconds = microseconds;
                
                #ifdef _MSC_VER 
                sprintf_s(filepath, sizeof(filepath),"Image_%s_w%d_h%d_fn%03d.jpg", m_mapSerials[nCurCameraIndex].c_str(), stParam.nWidth, stParam.nHeight, m_stImagesInfo[nCurCameraIndex].nFrameNum);
                FILE* fp ;
                fopen_s(&fp, filepath, "wb");
                #else
                sprintf(filepath,"Image_%s_w%d_h%d_fn%03d.jpg", m_mapSerials[nCurCameraIndex].c_str(), stParam.nWidth, stParam.nHeight, m_stImagesInfo[nCurCameraIndex].nFrameNum);
                FILE* fp = fopen( filepath, "wb");
                #endif               

                
                if (fp == NULL)
                {
                    printf("fopen failed\n");
                    break;
                }
                fwrite(m_pDataForSaveImages[nCurCameraIndex].get(), 1, stParam.nImageLen, fp);
                fclose(fp);

                #ifdef _MSC_VER 
                DEBUG_PRINT("%d. Camera, Save image succeeded, nFrameNum[%d], DeviceTimeStamp[%.3f ms], TimeDiff[%.3f ms], SystemTimeStamp[%lld ms], SystemTimeDiff[%.3f ms]\n", nCurCameraIndex,m_stImagesInfo[nCurCameraIndex].nFrameNum, double(timeStamp)/1000000, float(timeDif)/1000000,  uint64_t(round(double(microseconds)/1000)), double(systemTimeDiff)/1000);
                #else
                DEBUG_PRINT("%d. Camera, Save image succeeded, nFrameNum[%d], DeviceTimeStamp[%.3f ms], TimeDiff[%.3f ms], SystemTimeStamp[%ld ms], SystemTimeDiff[%.3f ms]\n", nCurCameraIndex,m_stImagesInfo[nCurCameraIndex].nFrameNum, double(timeStamp)/1000000, float(timeDif)/1000000,  uint64_t(round(double(microseconds)/1000)), double(systemTimeDiff)/1000);
                #endif
                
            }
            

            if (m_bExit) m_bStartConsuming = false;
            FPS_CALC ("Image Saving FPS:", nCurCameraIndex);

        }
    }
    return 0;
}

//Thread function with GetImageBuffer API
int HikMultipleCameras::ThreadGrabWithGetImageBufferFun(int nCurCameraIndex)
{
    int nRet = -1;
    if (m_pcMyCameras[nCurCameraIndex])
    {
        MV_FRAME_OUT stImageOut = {0};
        memset(&stImageOut, 0, sizeof(MV_FRAME_OUT));
        uint64_t oldtimeStamp = 0;
        long long int oldmicroseconds = 0;
        while(m_bStartGrabbing)
        {
            
            std::chrono::system_clock::time_point begin = std::chrono::system_clock::now();           
            nRet = m_pcMyCameras[nCurCameraIndex]->GetImageBuffer(&stImageOut, 1000);
            std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
            DEBUG_PRINT("Grabbing duration in DevIndex[%d]= %lld[ms]", nCurCameraIndex, std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() );
            
            if (nRet == MV_OK)
            {
                
                {
                    std::lock_guard<std::mutex> lk(m_mProduceMutexes[nCurCameraIndex]);

                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
                    std::chrono::microseconds ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
                    long long int microseconds = ms.count();

                    uint64_t timeStamp = (((uint64_t) stImageOut.stFrameInfo.nDevTimeStampHigh) << 32) + stImageOut.stFrameInfo.nDevTimeStampLow;
                    uint64_t  timeDif = timeStamp - oldtimeStamp;
                    uint64_t hostTimeStamp = stImageOut.stFrameInfo.nHostTimeStamp;
                    uint64_t systemTimeDiff = microseconds - oldmicroseconds;
                    oldtimeStamp = timeStamp; 
                    oldmicroseconds = microseconds;
                    #ifdef _MSC_VER 
                    DEBUG_PRINT("DevIndex[%d], Grab image succeeded, nFrameNum[%d], DeviceTimeStamp[%.3f ms], TimeDiff[%.3f ms], SystemTimeStamp[%lld ms], SystemTimeDiff[%.3f ms], HostTimeStamp[%lld ms]\n", nCurCameraIndex, stImageOut.stFrameInfo.nFrameNum, double(timeStamp)/1000000, float(timeDif)/1000000,  uint64_t(round(double(microseconds)/1000)), double(systemTimeDiff)/1000, hostTimeStamp);
                    #else
                    DEBUG_PRINT("DevIndex[%d], Grab image succeeded, nFrameNum[%d], DeviceTimeStamp[%.3f ms], TimeDiff[%.3f ms], SystemTimeStamp[%ld ms], SystemTimeDiff[%.3f ms], HostTimeStamp[%ld ms]\n", nCurCameraIndex, stImageOut.stFrameInfo.nFrameNum, double(timeStamp)/1000000, float(timeDif)/1000000,  uint64_t(round(double(microseconds)/1000)), double(systemTimeDiff)/1000, hostTimeStamp);
                    #endif
                    if (m_pSaveImagesBuf[nCurCameraIndex] == nullptr  || stImageOut.stFrameInfo.nFrameLen > m_nSaveImagesBufSize[nCurCameraIndex])
                    {
                       
                        m_pSaveImagesBuf[nCurCameraIndex].reset(new uint8_t[stImageOut.stFrameInfo.nFrameLen]);
                        if (m_pSaveImagesBuf[nCurCameraIndex] == nullptr)
                        {
                            printf("Failed to allocate memory! Exiting\n");                            
                            return -1;
                        }
                        m_nSaveImagesBufSize[nCurCameraIndex] = stImageOut.stFrameInfo.nFrameLen;
                    }


                    if (stImageOut.pBufAddr != NULL)
                    {   
    
                        memcpy(m_pSaveImagesBuf[nCurCameraIndex].get(), stImageOut.pBufAddr, stImageOut.stFrameInfo.nFrameLen);
                        memcpy(&(m_stImagesInfo[nCurCameraIndex]), &(stImageOut.stFrameInfo), sizeof(MV_FRAME_OUT_INFO_EX));                       
                        m_bImagesOk[nCurCameraIndex] = true;
                        nRet = m_pcMyCameras[nCurCameraIndex]->FreeImageBuffer(&stImageOut);
                        if (MV_OK != nRet)
                        {
                            printf("cannot free buffer! \n");
                            return nRet;
                        }
                     
                    }
                }
                m_cDataReadyCon[nCurCameraIndex].notify_one();
             
            } else {

                printf("Get Image Buffer fail! DevIndex[%d], nRet[%#x], \n", nCurCameraIndex, nRet);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            
            if (m_bExit) m_bStartGrabbing = false;
            FPS_CALC ("Image Grabbing FPS:", nCurCameraIndex);
        }

    }
    if (nRet == -1) 
    {
        printf("There is something wrong with the device opened in ThreadGrabWithGetImageBufferFun! DevIndex[%d] \n", nCurCameraIndex);
    }
    return nRet;
}




// Thread function with GetOneFrameTimeOut API
int HikMultipleCameras::ThreadGrabWithGetOneFrameFun(int nCurCameraIndex)
{
    int nRet = -1;
    if (m_pcMyCameras[nCurCameraIndex])
    {
        
        uint64_t oldtimeStamp = 0;
        MV_FRAME_OUT_INFO_EX stImageOut = {0};
        memset(&stImageOut, 0, sizeof(MV_FRAME_OUT_INFO_EX));

        while(m_bStartGrabbing)
        {
           
            if (m_pSaveImagesBuf[nCurCameraIndex] == nullptr )
            {
                
                m_pSaveImagesBuf[nCurCameraIndex].reset(new uint8_t[m_params[nCurCameraIndex].nCurValue]);
                if (m_pSaveImagesBuf[nCurCameraIndex] == nullptr)
                {
                    printf("Failed to allocate memory! Exiting\n");
                    return -1;
                }
            }
            
            std::chrono::system_clock::time_point begin = std::chrono::system_clock::now();
            nRet = m_pcMyCameras[nCurCameraIndex]->GetOneFrame(m_pSaveImagesBuf[nCurCameraIndex].get(), m_params[nCurCameraIndex].nCurValue, &stImageOut, 1000);
            std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
            #ifdef _MSC_VER 
            DEBUG_PRINT("Grabbing duration in DevIndex[%d]= %lld[ms]\n", nCurCameraIndex, std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() );
            #else
            DEBUG_PRINT("Grabbing duration in DevIndex[%d]= %ld[ms]\n", nCurCameraIndex, std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() );
            #endif

            if (nRet == MV_OK)
            {
                
                
                {
                    std::lock_guard<std::mutex> lk(m_mProduceMutexes[nCurCameraIndex]);
                    memcpy(&(m_stImagesInfo[nCurCameraIndex]), &(stImageOut), sizeof(MV_FRAME_OUT_INFO_EX)); 
                    m_bImagesOk[nCurCameraIndex] = true;
                    if (m_pSaveImagesBuf[nCurCameraIndex]) {
                       
                       // m_pSaveImageBuf[nCurCameraIndex].reset();

                    }
                  
                }
                m_cDataReadyCon[nCurCameraIndex].notify_one();
                
            }
            else
            {
                printf("Get Frame out Fail! DevIndex[%d], nRet[%#x]\r\n", nCurCameraIndex, nRet);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
            }

            if (m_bExit) m_bStartGrabbing = false;
            FPS_CALC ("Image Grabbing FPS:", nCurCameraIndex);
        }
    }
    if (nRet == -1) 
    {
        printf("There is something wrong with the device opened in ThreadGrabWithGetImageBufferFun! DevIndex[%d] \n", nCurCameraIndex);
    }
    return nRet;
}

void HikMultipleCameras::EnumDevicesByIPAddress()
{
    memset(&m_stDevList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

   
   
    unsigned int nIp1 = 172, nIp2 = 18, nIp3 = 166, nIp4 = 45, nIp;
    unsigned int n_ExIp1 = 172, n_ExIp2 = 18, n_ExIp3 = 166, n_ExIp4 = 76, n_ExIp;
    int k = 0;
    for (int i = 0 ; i < 1 ; i++)
    {
        
        MV_GIGE_DEVICE_INFO stGigEDev = {0};
        n_ExIp =  (n_ExIp1 << 24) | (n_ExIp2 << 16) | (n_ExIp3 << 8) | n_ExIp4;
        stGigEDev.nNetExport = n_ExIp;
        n_ExIp4++;

        for (int j = 0; j < 2; j++) {
            MV_CC_DEVICE_INFO stDevInfo = {0};
            nIp = (nIp1 << 24) | (nIp2 << 16) | (nIp3 << 8) | nIp4;
            stGigEDev.nCurrentIp = nIp;
            stDevInfo.nTLayerType = MV_GIGE_DEVICE;
            stDevInfo.SpecialInfo.stGigEInfo = stGigEDev;
            m_stDevList.pDeviceInfo[k] = new MV_CC_DEVICE_INFO (stDevInfo);
            nIp4++; k++;



        }
    }
    
    
    



    // int nRet = HikCamera::EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &m_stDevList);
    // if ( nRet != MV_OK || m_stDevList.nDeviceNum == 0)
    // {
    //     printf("Find no device!\r\n");
    //     return;
    // }
    // printf("Find %d devices!\r\n", m_stDevList.nDeviceNum);
    m_nDeviceNum = k;



   
    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        
        MV_CC_DEVICE_INFO* pDeviceInfo = m_stDevList.pDeviceInfo[i];
        m_pcMyCameras.push_back(std::make_unique<HikCamera>());
        int nRet = m_pcMyCameras[i]->CreateHandle(pDeviceInfo);
        if (MV_OK != nRet)
        {
            printf("Create Handle fail! DevIndex[%d], nRet[0x%x]\n",i, nRet);
            break;
        }



       
    }

    
}

void HikMultipleCameras::EnumDevices()
{
    memset(&m_stDevList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = HikCamera::EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &m_stDevList);
    if ( nRet != MV_OK || m_stDevList.nDeviceNum == 0)
    {
        printf("Find no device!\r\n");
        return;
    }
    printf("Find %d devices!\r\n", m_stDevList.nDeviceNum);
    
    m_nDeviceNum =  m_stDevList.nDeviceNum;
   
    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        
        MV_CC_DEVICE_INFO* pDeviceInfo = m_stDevList.pDeviceInfo[i];
        if (pDeviceInfo->nTLayerType == MV_GIGE_DEVICE)
        {
            int nIp1 = ((pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0xff000000) >> 24);
            int nIp2 = ((pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0x00ff0000) >> 16);
            int nIp3 = ((pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0x0000ff00) >> 8);
            int nIp4 = (pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0x000000ff);
            // Print the IP address and user defined name of the current camera
            DEBUG_PRINT("Device Model Name: %s\n", pDeviceInfo->SpecialInfo.stGigEInfo.chModelName);
            DEBUG_PRINT("CurrentIp: %d.%d.%d.%d\n" , nIp1, nIp2, nIp3, nIp4);
          //  printf("UserDefinedName: %s\n\n" , pDeviceInfo->SpecialInfo.stGigEInfo.chUserDefinedName);
            DEBUG_PRINT("SerialNumber: %s\n\n", pDeviceInfo->SpecialInfo.stGigEInfo.chSerialNumber);
            m_mapSerials.insert(std::make_pair(i , (char *)pDeviceInfo->SpecialInfo.stGigEInfo.chSerialNumber));
            m_mapModels.insert(std::make_pair(i, (char *) pDeviceInfo->SpecialInfo.stGigEInfo.chModelName));
            
        }
        else if (pDeviceInfo->nTLayerType == MV_USB_DEVICE)
        {
            
            DEBUG_PRINT("Device Model Name: %s\n", pDeviceInfo->SpecialInfo.stUsb3VInfo.chModelName);
            DEBUG_PRINT("UserDefinedName: %s\n\n", pDeviceInfo->SpecialInfo.stUsb3VInfo.chUserDefinedName);
            m_mapSerials.insert(std::make_pair( i, (char *)pDeviceInfo->SpecialInfo.stUsb3VInfo.chSerialNumber));
            m_mapModels.insert(std::make_pair(i, (char *) pDeviceInfo->SpecialInfo.stUsb3VInfo.chModelName));

        }
        else {
            printf("Camera not supported!\n");
        }    
    
        
       
    }

}


//  Initialzation, include opening device
void HikMultipleCameras::OpenDevices()
{
    if (true == m_bOpenDevice || m_nDeviceNum == 0)
    {
        printf("'m_bOpenDevice'set to true Or 'm_nDeviceNum' set to 0! Exiting from OpenDevices... \n");
        return;
    }
    
   
    
    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
       // m_pcMyCamera.push_back(std::make_unique<HikCamera>());
        //std::cout<<"step 0"<<std::endl;

        int nRet = m_pcMyCameras[i]->Open(m_stDevList.pDeviceInfo[i]);
        if (nRet != MV_OK)
        {
            m_pcMyCameras[i].reset();
            printf("Open device failed! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
            return;
        }
        else
        {
            
                        
            // Detect the optimal packet size (it is valid for GigE cameras only)
            m_bOpenDevice = true;
            if (m_stDevList.pDeviceInfo[i]->nTLayerType == MV_GIGE_DEVICE)
            {
                unsigned int  nPacketSize = 0;
                nRet = m_pcMyCameras[i]->GetOptimalPacketSize(&nPacketSize);
                if (nPacketSize > 0)
                {
                    nRet = m_pcMyCameras[i]->SetIntValue("GevSCPSPacketSize",nPacketSize);
                    if(nRet != MV_OK)
                    {
                        printf("Set Packet Size fail! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
                    }
                }
                else
                {
                    printf("Get Packet Size fail! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
                }
            }

            
        }
        
    }


   
}


void HikMultipleCameras::OpenDevicesInThreads()
{

    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
            m_tOpenDevicesThreads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadOpenDevicesFun, this, i)));
       
    }

}

int HikMultipleCameras::ThreadOpenDevicesFun(int nCurCameraIndex) 
{
    if (true == m_bOpenDevice || m_nDeviceNum == 0)
    {
        printf("'m_bOpenDevice'set to true Or 'm_nDeviceNum' set to 0! Exiting from OpenDevices... \n");
        return -1;
    }
   
   
    
    int nRet = m_pcMyCameras[nCurCameraIndex]->Open(m_stDevList.pDeviceInfo[nCurCameraIndex]);
    if (nRet != MV_OK)
    {
        m_pcMyCameras[nCurCameraIndex].reset();
        printf("Open device failed! DevIndex[%d], nRet[%#x]\r\n", nCurCameraIndex , nRet);
        return -1;
    }
    else
    {
        
                    
        // Detect the optimal packet size (it is valid for GigE cameras only)
        m_bOpenDevice = true;
        if (m_stDevList.pDeviceInfo[nCurCameraIndex]->nTLayerType == MV_GIGE_DEVICE)
        {
            unsigned int nPacketSize = 0;
            nRet = m_pcMyCameras[nCurCameraIndex]->GetOptimalPacketSize(&nPacketSize);
            if (nPacketSize > 0)
            {
                nRet = m_pcMyCameras[nCurCameraIndex]->SetIntValue("GevSCPSPacketSize",nPacketSize);
                if(nRet != MV_OK)
                {
                    printf("Set Packet Size fail! DevIndex[%d], nRet[%#x]\r\n", nCurCameraIndex, nRet);
                }
            }
            else
            {
                printf("Get Packet Size fail! DevIndex[%d], nRet[%#x]\r\n", nCurCameraIndex, nRet);
            }
        }

        
    }
    

    return MV_OK;
        



}

void HikMultipleCameras::JoinOpenDevicesInThreads() {

    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            m_tOpenDevicesThreads[i]->join();
        }
    }


}


void HikMultipleCameras::CloseDevicesInThreads()
{

    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            m_tCloseDevicesThreads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadCloseDevicesFun, this, i)));
        }
    }

}

int HikMultipleCameras::ThreadCloseDevicesFun(int nCurCameraIndex)
{

    int nRet = -1;
   
    if (m_pcMyCameras[nCurCameraIndex])
    {
        nRet = m_pcMyCameras[nCurCameraIndex]->Close();
        if (MV_OK != nRet)
        {
            printf("Close device fail! DevIndex[%d], nRet[%#x]\r\n", nCurCameraIndex, nRet);
        }
        else {

            printf("Close device success! DevIndex[%d], nRet[%#x]\r\n", nCurCameraIndex, nRet);
        }

        
        
    }

    return nRet; 

}
void HikMultipleCameras::JoinCloseDevicesInThreads() {

    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            m_tCloseDevicesThreads[i]->join();
        }
    }


}


int HikMultipleCameras::ConfigureCameraSettings()
{
    boost::property_tree::ptree pt;

    // Read the JSON file
    std::ifstream file(m_sCameraSettingsFile);
    if (!file.good()) 
    {
        printf("Error in opening 'CameraSettings.json' file! Exiting... \n");
        return -1;
    }

    boost::property_tree::read_json(file, pt);
    m_sTriggerSource = pt.get<std::string>("TriggerSource");
    m_nTriggerTimeInterval = pt.get<int>("TriggerTimeInterval");
    m_pBroadcastAddress = pt.get<std::string>("BroadcastAddress");
    int height = pt.get<int>("Height");
    int width = pt.get<int>("Width");
    int exposureAuto = pt.get<int>("ExposureAuto");
    float exposureTime = pt.get<float>("ExposureTime");
    bool acquisitionFrameRateEnable = pt.get<bool>("AcquisitionFrameRateEnable");
    float acquisitionFrameRate = pt.get<float>("AcquisitionFrameRate");
    bool gevPAUSEFrameReception = pt.get<bool>("GevPAUSEFrameReception");
    bool gevIEEE1588 = pt.get<bool>("GevIEEE1588");
    float gain = pt.get<float>("Gain");
   
    
    file.close();
    
 
    
    int nRet = -1;
    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i]) 
        {
            nRet = m_pcMyCameras[i]->SetIntValue("Height", height);
            if (nRet != MV_OK){
                printf("Cannot set Height fail! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet ;
            }
            nRet = m_pcMyCameras[i]->SetIntValue("Width", width);
            if (nRet != MV_OK){
                printf(" Cannot set Width! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet ;
            }

            nRet = m_pcMyCameras[i]->SetEnumValue("ExposureAuto", exposureAuto);
            if (nRet != MV_OK)
            {
                printf("Cannot set Exposure Auto value! DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet ;
            }

            nRet = m_pcMyCameras[i]->SetFloatValue("ExposureTime", exposureTime);
            if (nRet != MV_OK)
            {
                printf("Cannot set Exposure Time value! DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet;
            }
            nRet = m_pcMyCameras[i]->SetBoolValue("AcquisitionFrameRateEnable", acquisitionFrameRateEnable);
            if (nRet != MV_OK)
            {
                printf("Cannot set Acquisition FrameRate Enable!. DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet ;
            }

            nRet = m_pcMyCameras[i]->SetFloatValue("AcquisitionFrameRate", acquisitionFrameRate); 
            if (nRet != MV_OK)
            {
                printf("Cannot set Acquisition Frame Rate value! DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet;
            }
        
            nRet = m_pcMyCameras[i]->SetBoolValue("GevPAUSEFrameReception", gevPAUSEFrameReception);
            if (nRet != MV_OK)
            {
                printf("Cannot set GevPAUSEFrameReception Acquisition Enable! DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet;
            }
        
            nRet = m_pcMyCameras[i]->SetBoolValue("GevIEEE1588", gevIEEE1588);
            if (nRet != MV_OK)
            {
                printf("Cannot set  GevIEEE1588 Enable! DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet;
            }
            nRet = m_pcMyCameras[i]->SetFloatValue("Gain", gain);

            if (nRet != MV_OK) 
            {
                printf("Cannot set  Gain Value! DevIndex[%d], nRet[%#x]. Exiting...\r\n ", i, nRet);
                return nRet;
            }
           
            
        }
    }
    if (nRet == -1) 
    {
        printf("There is something wrong with the number of opened devices in ConfigureCameraSettings! \n");
    }
    return nRet;

}

// Opening threads for resetting timestamp control
void HikMultipleCameras::OpenThreadsTimeStampControlReset()
{

    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
            if (m_pcMyCameras[i])
            {
               m_tResetTimestampThreads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadTimeStampControlResetFun, this, i)));
            }
            

    }
}

// Thread function for resetting timestamp control
int HikMultipleCameras::ThreadTimeStampControlResetFun(int nCurCameraIndex) {

    if (m_pcMyCameras[nCurCameraIndex])
    {
        int nRet;
        
        // if ( m_mapModels[nCurCameraIndex] == std::string("MV-CA023-10GC")) 
        // {
        //    // std::this_thread::sleep_for(std::chrono::milliseconds(14));
        // }
        nRet = m_pcMyCameras[nCurCameraIndex]->CommandExecute("GevTimestampControlReset") ;
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        std::chrono::microseconds ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
        long long int microseconds = ms.count();
        printf("Time Stamp of  %d. Camera: %lld\n", nCurCameraIndex, microseconds);

        if (nRet != MV_OK) 
        {
            printf("%d. Camera, TimeStampControlReset failed! \n", nCurCameraIndex);
            return -1;
        }
       return 0; 
    }

    return -1;
}

// Join threads for resetting timestamp control
void HikMultipleCameras::JoinThreadsTimeStampControlReset()
{

    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            m_tResetTimestampThreads[i]->join();
        }
    }
}

void HikMultipleCameras::TimeStampControlReset() 
{
    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            int nRet;
            nRet = m_pcMyCameras[i]->CommandExecute("GevTimestampControlReset") ;
            if (nRet != MV_OK) printf("%d. Camera, TimeStampControlReset failed! \n", i);
        }
    }
}



// Close, include destroy handle
void HikMultipleCameras::CloseDevices()
{

	int nRet = -1;
    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            nRet = m_pcMyCameras[i]->Close();
            if (MV_OK != nRet)
            {
                printf("Close device fail! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
            }
            else {

                printf("Close device success! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
            }

            
          
        }

    }

    
}


int HikMultipleCameras::Save2BufferThenDisk()
{
    
    if (false == m_bStartGrabbing)
    {        
        printf("'m_bStartGrabbing' set to false! Cannot save images. \n");
        return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {

            m_tSaveBufThreads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadSave2BufferFun, this, i)));
            if (m_tSaveBufThreads[i] == nullptr)
            {
                printf("Create Save Buffer thread fail! DevIndex[%d]\r\n", i);
                return -1;
            }

            m_tWriteMP4Threads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadWrite2MP4Fun, this, i)));

        }
    }

  //  m_tSaveBufThread = std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadSave2BufferFun, this));
    m_tCheckBuffThread = std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadCheckBufferFun, this));

    m_tSaveDiskThread =  std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadSave2DiskFun, this));
   
    return MV_OK;

}   

int HikMultipleCameras::ThreadCheckBufferFun() 
{
    while(true)
    {

        std::unique_lock<std::mutex> lk(m_mGrabMutex);
        m_cDataReadySingleCon1.wait(lk, [this] {
            bool sum=true;
            int i = 0;
            for (; i < m_bImagesCheck.size(); i++){
                
                sum = sum && m_bImagesCheck[i];

            }
            if (i == 0 ) return false;
            else return sum;

        });

        for (int i =0 ; i < m_bImagesCheck.size(); i++)
            m_bImagesCheck[i]= false;
       
        if (!m_buf.pushBack(m_pairImagesInfo_Buff) )
        {
            
            printf ("Warning! Buffer was full, overwriting data!\n");
            
        }
            

        

        FPS_CALC_BUF ("image callback.", m_buf);

        if (m_bExit)
            break;
        
    
    }
    return 0;

}

int HikMultipleCameras::ThreadSave2BufferFun(int nCurCameraIndex)
{
    if (m_pcMyCameras[nCurCameraIndex])
    {   

        while(true)
        {
            {
             
                std::unique_lock<std::mutex> lk(m_mProduceMutexes[nCurCameraIndex]);
                m_cDataReadyCon[nCurCameraIndex].wait(lk, [this, nCurCameraIndex] {
                    return m_bImagesOk[nCurCameraIndex];

                });
               
                if  (m_pSaveImagesBuf[nCurCameraIndex] == nullptr) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    printf("continue \n");
                    continue;
                }
                std::shared_ptr<uint8_t[]> clonedSharedPtr (new uint8_t[m_stImagesInfo[nCurCameraIndex].nFrameLen]);
                memcpy(clonedSharedPtr.get(), m_pSaveImagesBuf[nCurCameraIndex].get(),  m_stImagesInfo[nCurCameraIndex].nFrameLen * sizeof(uint8_t));
                m_pairImagesInfo_Buff[nCurCameraIndex] = std::make_pair(m_stImagesInfo[nCurCameraIndex], clonedSharedPtr);
                m_bImagesOk[nCurCameraIndex] = false;
                m_bImagesCheck[nCurCameraIndex] = true;


              
            }

            m_cDataReadySingleCon1.notify_one();

            

            if (m_bExit) break;
            FPS_CALC ("Image Saving To Buffer FPS:", nCurCameraIndex);

        }

    }  
    return 0;
}

int HikMultipleCameras::ThreadSave2DiskFun(){

    while (true)
    {
        if (m_bExit) break;
        {
            std::unique_lock<std::mutex> lk(m_mSaveMutex);
            m_cDataReadySingleCon2.wait(lk, [this] {
                bool sum = true;
                unsigned int i = 0;
                for (; i < m_bImagesReady.size(); i++){
                    
                    sum = sum && m_bImagesReady[i];

                }
                if (i == 0 ) return false;
                else return sum;

            });
            for (unsigned int i =0 ; i < m_bImagesCheck.size(); i++)
                m_bImagesReady[i]= false;

            m_currentPairImagesInfo_Buff = &(m_buf.getFront());
            
        }
        

    }

    while (!m_buf.isEmpty ()) 
    {
        {
            std::unique_lock<std::mutex> lk(m_mSaveMutex);
            m_cDataReadySingleCon2.wait(lk, [this] {
                bool sum = true;
                unsigned int i = 0;
                for (; i < m_bImagesReady.size(); i++){
                    
                    sum = sum && m_bImagesReady[i];

                }
                if (i == 0 ) return false;
                else return sum;

            });
            for (unsigned int i =0 ; i < m_bImagesCheck.size(); i++)
                m_bImagesReady[i]= false;

            m_currentPairImagesInfo_Buff = &(m_buf.getFront());
        
        }

       printf("Buffer size:%d\n",m_buf.getSize());  
    }

   
    
    
    // while (true)
    // {
    //     if (m_bExit)   break;
    //     Write2MP4(m_buf.getFront ());
    // }

      
    // while (!m_buf.isEmpty ()) {

    //     Write2MP4(m_buf.getFront ());
    //     printf("Buffer size:%d\n",m_buf.getSize());
    // }

    return 0;
}
void HikMultipleCameras::Write2Disk( const std::vector<std::pair<MV_FRAME_OUT_INFO_EX, std::shared_ptr<uint8_t[]>>> & buff_item){
    unsigned char * pDataForSaveImage = NULL;
    for (int i = 0 ; i < buff_item.size(); i++)
    {
        MV_SAVE_IMAGE_PARAM_EX stParam;
        memset(&stParam, 0, sizeof(MV_SAVE_IMAGE_PARAM_EX));

        if ( !pDataForSaveImage) 
            pDataForSaveImage = (unsigned char*)malloc(m_stImagesInfo[i].nWidth * m_stImagesInfo[i].nHeight * 4 + 2048);


        stParam.enImageType = MV_Image_Jpeg; 
        stParam.enPixelType =  buff_item[i].first.enPixelType; 
        stParam.nWidth = buff_item[i].first.nWidth;       
        stParam.nHeight = buff_item[i].first.nHeight;       
        stParam.nDataLen = buff_item[i].first.nFrameLen;
        stParam.pData = buff_item[i].second.get();
        stParam.pImageBuffer =  pDataForSaveImage;
        stParam.nBufferSize = buff_item[i].first.nWidth * buff_item[i].first.nHeight * 4 + 2048;;  
        stParam.nJpgQuality = 99;  

        int nRet =  m_pcMyCameras[i]->SaveImage(&stParam);

        if(nRet != MV_OK)
        {
            printf("Failed in MV_CC_SaveImage,nRet[%x]\n", nRet);
           // std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        char filepath[256];
        
     

        uint64_t timeStamp = (((uint64_t)  buff_item[i].first.nDevTimeStampHigh) << 32) +  buff_item[i].first.nDevTimeStampLow;

        #ifdef _MSC_VER 
        sprintf_s(filepath, sizeof(filepath), "Image_%s_w%d_h%d_fn%03d.jpg", m_mapSerials[i].c_str(), stParam.nWidth, stParam.nHeight,  buff_item[i].first.nFrameNum);
        FILE* fp;
        fopen_s(&fp, filepath, "wb");
        #else
        sprintf(filepath, "Image_%s_w%d_h%d_fn%03d.jpg", m_mapSerials[i].c_str(), stParam.nWidth, stParam.nHeight,  buff_item[i].first.nFrameNum);
        FILE* fp = fopen(filepath, "wb");
        #endif
      
        if (fp == NULL)
        {
            printf("fopen failed\n");
            break;
        }
        fwrite(pDataForSaveImage, 1, stParam.nImageLen, fp);
        fclose(fp);
       // DEBUG_PRINT("%d. Camera, Save image succeeded, nFrameNum[%d], DeviceTimeStamp[%.3f ms], TimeDiff[%.3f ms], SystemTimeStamp[%ld ms], SystemTimeDiff[%.3f ms]\n", i,m_stImagesInfo[i].nFrameNum, double(timeStamp)/1000000, float(timeDif)/1000000,  uint64_t(round(double(microseconds)/1000)), double(systemTimeDiff)/1000);




    }
    delete pDataForSaveImage;


}



int HikMultipleCameras::ThreadWrite2MP4Fun( int nCurCameraIndex){

    while (true)
    {
        if (m_currentPairImagesInfo_Buff == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        int arraySize = m_currentPairImagesInfo_Buff->at(nCurCameraIndex).first.nFrameLen;
        if (!m_Containers[nCurCameraIndex].writeImageToContainer((char*) m_currentPairImagesInfo_Buff->at(nCurCameraIndex).second.get(), arraySize, m_its[nCurCameraIndex]*100000, STREAM_INDEX_IMG)) 
        {
            printf("Cannot write texture to container\n");
            return -1 ;
        }
        m_its[nCurCameraIndex]++;
        m_bImagesReady[nCurCameraIndex] = true;
        m_cDataReadySingleCon2.notify_one();
        if (m_bExit) break;
       
    }
   
    return MV_OK;

}

void HikMultipleCameras::Write2MP4( const std::vector<std::pair<MV_FRAME_OUT_INFO_EX, std::shared_ptr<uint8_t[]>>> & buff_item)
{
    unsigned char * pDataForSaveImage = NULL;
    for (int i = 0 ; i < buff_item.size(); i++)
    {
        MV_SAVE_IMAGE_PARAM_EX stParam;
        memset(&stParam, 0, sizeof(MV_SAVE_IMAGE_PARAM_EX));

        if ( !pDataForSaveImage) 
            pDataForSaveImage = (unsigned char*)malloc(m_stImagesInfo[i].nWidth * m_stImagesInfo[i].nHeight * 4 + 2048);


        // stParam.enImageType = MV_Image_Jpeg; 
        // stParam.enPixelType =  buff_item[i].first.enPixelType; 
        // stParam.nWidth = buff_item[i].first.nWidth;       
        // stParam.nHeight = buff_item[i].first.nHeight;       
        // stParam.nDataLen = buff_item[i].first.nFrameLen;
        // stParam.pData = buff_item[i].second.get();
        // stParam.pImageBuffer =  pDataForSaveImage;
        // stParam.nBufferSize = buff_item[i].first.nWidth * buff_item[i].first.nHeight * 4 + 2048;;  
        // stParam.nJpgQuality = 99;  
        // std::chrono::system_clock::time_point begin = std::chrono::system_clock::now();           
        
            
        // //int nRet =  m_pcMyCameras[i]->SaveImage(&stParam);
        // std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
        // printf("Duration in Save Image DevIndex[%d]= %ld[ms]\n", i, std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() );


        // if(nRet != MV_OK)
        // {
        //     printf("Failed in MV_CC_SaveImage,nRet[%x]\n", nRet);
        //    // std::this_thread::sleep_for(std::chrono::milliseconds(5));
        //     continue;
        // }
        int arraySize = buff_item[i].first.nFrameLen;
        if (!m_Container.writeImageToContainer((char*) buff_item[i].second.get(), arraySize, m_it*100000, STREAM_INDEX_IMG)) 
        {
            printf("Cannot write texture to container\n");
            return ;
		}
		m_it++;




    }

}


// Start grabbing
int HikMultipleCameras::StartGrabbing()
{
    if (m_bStartGrabbing == true)
    {        
        printf("'m_bStartGrabbing' already set to true! Exiting... \n");
        return -1;
    }

   
    int nRet = -1;

    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            memset(&(m_stImagesInfo[i]), 0, sizeof(MV_FRAME_OUT_INFO_EX));
            
            
            nRet = m_pcMyCameras[i]->StartGrabbing();
            if (MV_OK != nRet)
            {
                printf("Start grabbing fail! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }
            m_bStartGrabbing = true;

            memset(&m_params[i], 0, sizeof(MVCC_INTVALUE));
            nRet =  m_pcMyCameras[i]->GetIntValue("PayloadSize", &m_params[i]);
            if (nRet != MV_OK) {
                printf("Get PayloadSize failed! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }

           
            m_tGrabThreads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadGrabWithGetImageBufferFun, this, i)));
           
            if (m_tGrabThreads[i] == nullptr)
            {
                printf("Create grab thread fail! DevIndex[%d]. Exiting...\r\n", i);
                return -1;
            }
        }
    }
    
    std::this_thread::sleep_until(m_timePoint + std::chrono::milliseconds(10));
    if (m_nTriggerMode == MV_TRIGGER_MODE_ON)
    {
        if (m_sTriggerSource == "Action1")
            m_tTriggerThread = std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadTriggerGigActionCommandFun, this));
        else if (m_sTriggerSource == "Software")
            m_tTriggerThread = std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadSoftwareTriggerFun, this));
        else {
            printf("Only GigE Action Command Trigger and Softare Trigger supported! Exiting... \n");
            return -1;
        }
    }

// Consider including the line below incase of saving in buffer.
    // m_grabThread = new std::thread(std::bind(&HikMultipleCameras::SaveToBuffer, this));
   
    
    if (nRet == -1) 
    {
        printf("There is something wrong with the number of opened devices in StartGrabbing! \n");
    }
    return nRet;

}

// Thread function for triggering
int HikMultipleCameras::ThreadSoftwareTriggerFun() 
{

    while(true) 
    {
      
        std::this_thread::sleep_until(m_timePoint);
        
        for (unsigned int  i = 0; i < m_nDeviceNum; i++)
        {
                if (m_pcMyCameras[i])
                {
                    m_pcMyCameras[i]->CommandExecute("TriggerSoftware");
                }

        }
           
        m_timePoint += std::chrono::milliseconds(m_nTriggerTimeInterval);

        
        if (m_bExit) break;
    }
    return 0;
}

// Thread function for triggering with mutex
int HikMultipleCameras::ThreadTriggerGigActionCommandFun() 
{
    
    int nRet = -1;
    while(true) 
    {
        
        
            nRet = HikCamera::GIGEIssueActionCommand(&m_actionCMDInfo, &m_actionCMDResList);
            if (MV_OK != nRet)
            {
                printf("Issue Action Command fail! nRet [0x%x]\n", nRet);
                continue;
            }
            printf("NumResults = %d\r\n",m_actionCMDResList.nNumResults);

            MV_ACTION_CMD_RESULT* pResults = m_actionCMDResList.pResults;
            for (unsigned int i = 0;i < m_actionCMDResList.nNumResults;i++)
            {
                //Print the device infomation
                DEBUG_PRINT("Ip == %s, Status == 0x%x\r\n",pResults->strDeviceAddress,pResults->nStatus);
                pResults++;
            }
       
        if (m_bExit) break;
       
    }
    return nRet;
}



// Stop grabbing
int HikMultipleCameras::StopGrabbing()
{
    if ( m_bOpenDevice == false || m_bStartGrabbing == false)
    {        
        printf("'m_bOpenDevice'set to false Or 'm_bStartGrabbing' set to false! Exiting from StopGrabbing... \n");
        return -1;
    }

    if (m_nTriggerMode == MV_TRIGGER_MODE_ON)
        m_tTriggerThread->join();
    

    int nRet = -1, nRetOne;
    bool bRet = false;
   // m_tSaveBufThread->join();

    // m_tCheckBuffThread->join();
    // m_tSaveDiskThread->join();

    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            
            m_tGrabThreads[i]->join();
            m_tConsumeThreads[i]->join();
            // m_tSaveBufThreads[i]->join();
            // m_tWriteMP4Threads[i]->join();
            
            nRet = m_pcMyCameras[i]->StopGrabbing();
            if (MV_OK != nRet)
            {
                printf("Stop grabbing fail! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
                bRet = true;
                nRetOne = nRet;
            } else {

                printf("Stop grabbing success! DevIndex[%d], nRet[%#x]\r\n", i, nRet);
            }

          //  m_Containers[i].close();
          //  m_Containers[i].openForRead((char*)m_Containers[i].file_name);
          //  m_tReadMp4Threads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadReadMp4Fun, this, i)));
        }
        
    }


   
    if (nRet == -1) 
    {
        printf("There is something wrong with the number of opened devices in StopGrabbing! \n");
        return nRet;
    }
    if (bRet)
    {
        printf("Cannot stop grabbing for at least one camera\n");
        return nRetOne;

    }
    return nRet;
   

}

int HikMultipleCameras::ThreadReadMp4Fun(int nCurCameraIndex ) 
{
    int length;
    char *data;
    unsigned char * pDataForSaveImage = NULL;

    while (true) 
    {

        
        int streamIndex = m_Containers[nCurCameraIndex].read(data, length);
        if (streamIndex == -1) 
        {
            printf("Cannot read data from Mp4 Or End of file, DevIndex[%d]\n", nCurCameraIndex);
            break;
        }
        // if ( !pDataForSaveImage) 
        //         pDataForSaveImage = (unsigned char*)malloc(m_stImagesInfo[i].nWidth * m_stImagesInfo[i].nHeight * 4 + 2048);
        // MV_SAVE_IMAGE_PARAM_EX stParam;
        // memset(&stParam, 0, sizeof(MV_SAVE_IMAGE_PARAM_EX));

        // stParam.enImageType = MV_Image_Jpeg; 
        // stParam.enPixelType =  buff_item[i].first.enPixelType; 
        // stParam.nWidth = buff_item[i].first.nWidth;       
        // stParam.nHeight = buff_item[i].first.nHeight;       
        // stParam.nDataLen = length;
        // stParam.pData = (unsigned char*)data;
        // stParam.pImageBuffer =  pDataForSaveImage;
        // stParam.nBufferSize = buff_item[i].first.nWidth * buff_item[i].first.nHeight * 4 + 2048;;  
        // stParam.nJpgQuality = 99;  
        // int nRet =  m_pcMyCameras[i]->SaveImage(&stParam);

        // if(nRet != MV_OK)
        // {
        //     printf("Failed in MV_CC_SaveImage,nRet[%x]\n", nRet);
        //     // std::this_thread::sleep_for(std::chrono::milliseconds(5));
        //     continue;
        // }
        // char filepath[256];
        
        


        // #ifdef _MSC_VER 
        // sprintf_s(filepath, sizeof(filepath), "Image_%s_w%d_h%d_fn%03d.jpg", m_mapSerials[i].c_str(), stParam.nWidth, stParam.nHeight,  buff_item[i].first.nFrameNum);
        // FILE* fp;
        // fopen_s(&fp, filepath, "wb");
        // #else
        // sprintf(filepath, "Image_%s_w%d_h%d_fn%03d.jpg", m_mapSerials[i].c_str(), stParam.nWidth, stParam.nHeight,  buff_item[i].first.nFrameNum);
        // FILE* fp = fopen(filepath, "wb");
        // #endif
        
        // if (fp == NULL)
        // {
        //     printf("fopen failed\n");
        //     break;
        // }
        // fwrite(pDataForSaveImage, 1, stParam.nImageLen, fp);
        // fclose(fp);

    }   


    return 0;

}


// Set trigger mode on or off
int HikMultipleCameras::SetTriggerModeOnOff(int triggerMode)
{
    if (m_nDeviceNum == 0) return -1;

    m_nTriggerMode = triggerMode;
    int nRet =  SetTriggerMode();
    if (nRet != MV_OK)
        return nRet;

    if (m_nTriggerMode == MV_TRIGGER_MODE_ON)  
    {
        if (m_sTriggerSource == "Action1") 
        {
            m_timePoint =  std::chrono::system_clock::now();
            return SetTriggerGigEAction();

        } else if (m_sTriggerSource == "Software")
        {
            return SetTriggerSoftwareMode();
        } else{
            printf("Only GigE Action Command Trigger and Softare Trigger supported! Exiting... \n");
            return -1;
        }
    }
    return MV_OK;
}

// Software trigger
int HikMultipleCameras::SetTriggerSoftwareMode()
{
    int nRet = -1;
    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            nRet = m_pcMyCameras[i]->SetEnumValueByString("TriggerSource", m_sTriggerSource.c_str());
            if (nRet != MV_OK)
            {
                printf("Cannot set software Trigger! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }
        }
    }
    if (nRet == -1) 
    {
        printf("There is something wrong with the number of opened devices in SetTriggerSoftwareMode! Exiting... \n");
    }
    
    return nRet;
}

// GigE Action Command Trigger
int  HikMultipleCameras::SetTriggerGigEAction() 
{
    int nRet = -1;
    for (unsigned int  i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {
            nRet = m_pcMyCameras[i]->SetEnumValueByString("TriggerSource", m_sTriggerSource.c_str());
            if (nRet != MV_OK)
            {
                printf("Cannot set  Trigger GigE Action1! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }
            nRet = m_pcMyCameras[i]->SetIntValue("ActionDeviceKey", m_nDeviceKey);
            if (nRet != MV_OK)
            {
                printf("Cannot set Action Device Key! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }            

            nRet = m_pcMyCameras[i]->SetIntValue("ActionGroupMask", m_nGroupMask);
            if (nRet != MV_OK)
            {
                printf("Cannot set Action Group Mask! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }  
            nRet = m_pcMyCameras[i]->SetIntValue("ActionGroupKey", n_nGroupKey);
            if (nRet != MV_OK)
            {
                printf("Cannot set Action Group Key! DevIndex[%d], nRet[%#x]. Exiting...\r\n", i, nRet);
                return nRet;
            }  
            
        }
    }
    if (nRet == -1) 
    {
        printf("There is something wrong with the number of opened devices in SetTriggerGigEAction! Exiting... \n");
    }

    m_actionCMDInfo.nDeviceKey = m_nDeviceKey;
    m_actionCMDInfo.nGroupKey = n_nGroupKey;
    m_actionCMDInfo.nGroupMask = m_nGroupMask;
    m_actionCMDInfo.pBroadcastAddress = m_pBroadcastAddress.c_str();
    m_actionCMDInfo.nTimeOut = m_nTriggerTimeInterval;
    m_actionCMDInfo.bActionTimeEnable = 0;


    return nRet;

}


// Save Images of Cameras in threads
int HikMultipleCameras::SaveImages2Disk()
{
    
    if (false == m_bStartGrabbing)
    {        
        printf("'m_bStartGrabbing' set to false! Cannot save images. \n");
        return -1;
    }

    
    for (unsigned int i = 0; i < m_nDeviceNum; i++)
    {
        if (m_pcMyCameras[i])
        {

            m_bStartConsuming = true;
            m_tConsumeThreads.push_back(std::make_unique<std::thread>(std::bind(&HikMultipleCameras::ThreadConsumeFun, this, i)));
            if (m_tConsumeThreads[i] == nullptr)
            {
                printf("Create consume thread fail! DevIndex[%d]\r\n", i);
                return -1;
            }

        }
    }
    return MV_OK;

}   



