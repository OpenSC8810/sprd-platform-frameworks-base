/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.internal.telephony;

import android.content.pm.PackageManager;
import android.os.AsyncResult;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.ServiceManager;
import android.telephony.PhoneNumberUtils;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;

/**
 * SimPhoneBookInterfaceManager to provide an inter-process communication to
 * access ADN-like SIM records.
 */
public abstract class IccPhoneBookInterfaceManager extends IIccPhoneBook.Stub {
    protected static final boolean DBG = true;

    protected PhoneBase phone;
    protected AdnRecordCache adnCache;
    protected final Object mLock = new Object();
//zhanglj add begin for 
	protected final Object mUsimLock = new Object();
	protected final Object mEfLock = new Object();
	protected final Object mAdnLock = new Object();
	protected final Object mSyncLock = new Object();
//zhanglj add begin for
    protected int recordSize[];
    protected int sim_index = 0;	//yeezone:jinwei add sim manage
    protected boolean success;
    protected List<AdnRecord> records;

    protected static final boolean ALLOW_SIM_OP_IN_UI_THREAD = false;

    protected static final int EVENT_GET_SIZE_DONE = 1;
    protected static final int EVENT_LOAD_DONE = 2;
    protected static final int EVENT_UPDATE_DONE = 3;

    protected Handler mBaseHandler = new Handler() {
        @Override
        public void handleMessage(Message msg) {
            AsyncResult ar;

            switch (msg.what) {
                case EVENT_GET_SIZE_DONE:
                    ar = (AsyncResult) msg.obj;
                    synchronized (mLock) {
                        if (ar.exception == null) {
                            recordSize = (int[])ar.result;
                            // recordSize[0]  is the record length
                            // recordSize[1]  is the total length of the EF file
                            // recordSize[2]  is the number of records in the EF file
                            logd("GET_RECORD_SIZE Size " + recordSize[0] +
                                    " total " + recordSize[1] +
                                    " #record " + recordSize[2]);
                            mLock.notifyAll();
                        }
                    }
                    break;
                case EVENT_UPDATE_DONE:
                    ar = (AsyncResult) msg.obj;
                    synchronized (mLock) {
                        success = (ar.exception == null);
						//yeezone:jinwei get sim index from AsynResult object
                        if(success){
                            sim_index = Integer.valueOf(ar.result.toString());
                        }
						//end 
                        mLock.notifyAll();
                    }
                    break;
                case EVENT_LOAD_DONE:
                    ar = (AsyncResult)msg.obj;
                    synchronized (mLock) {
                        if (ar.exception == null) {
                            records = (List<AdnRecord>) ar.result;
                        } else {
                            if(DBG) logd("Cannot load ADN records");
                            if (records != null) {
                                records.clear();
                            }
                        }
                        mLock.notifyAll();
                    }
                    break;
            }
        }
    };

    public IccPhoneBookInterfaceManager(PhoneBase phone) {
        this.phone = phone;
    }

    public void dispose() {
    }

    protected void publish() {
        //NOTE service "simphonebook" added by IccSmsInterfaceManagerProxy
        ServiceManager.addService("simphonebook", this);
    }

    protected abstract void logd(String msg);

    protected abstract void loge(String msg);
	//yeezone:jinwei Add a new contact in SimCard.
     /**Add newAdn in ADN-like record in EF	 
      * @param efid must be one among EF_ADN, EF_FDN, and EF_SDN	 
      * @param newTag adn tag to be stored     
      * @param newPhoneNumber adn number ot be stored     
      *        Set both newTag and newPhoneNubmer to "" means to replace the old     
      *        record with empty one, aka, delete old record     
      * @param pin2 required to update EF_FDN, otherwise must be null     
      * @return sim_index	
     */
     public int addAdnRecordsInEf(int efid, String newTag, String newPhoneNumber, String pin2)
     {
        if (phone.getContext().checkCallingOrSelfPermission(
                android.Manifest.permission.WRITE_CONTACTS)
            != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(
                    "Requires android.permission.WRITE_CONTACTS permission");
        }
        Log.v("IccPhoneBookInterfaceManager", "addIccRecordToEf** ");
        if (DBG) logd("addAdnRecordsInEf: efid=" + efid +
                " ("+ newTag + "," + newPhoneNumber + ")"+ " pin2=" + pin2);

	  int newid =   updateEfForIccType(efid);

	  int fileId[] = adnCache.getRecordsEfId(newid);

	  for(int i=0; i<fileId.length;i++){
	  	
            if (DBG) logd("addAdnRecordsInEf: efid=" + fileId[i]);
	  }
        synchronized(mLock) {
            sim_index = 0;
            checkThread();
            Message response = mBaseHandler.obtainMessage(EVENT_UPDATE_DONE);
            AdnRecord oldAdn = new AdnRecord("", "");
            AdnRecord newAdn = new AdnRecord(newTag, newPhoneNumber);
            Log.i("IccPhoneBookInterfaceManager", "newTag="+newTag+"  newPhoneNumber="+newPhoneNumber);
            for(int i=0; i<fileId.length;i++){
	  	   
                if (DBG) logd("addAdnRecordsInEf: efid=" + fileId[i]);
				
		   if(adnCache.updateAdnBySearch(fileId[i], oldAdn, newAdn, pin2, response)){

			  break;
		   }
	     }
	
            try {
                mLock.wait();
            } catch (InterruptedException e) {
                logd("interrupted while trying to update by search");
            }
        }
        return sim_index;
    }
    /**
     * Replace oldAdn with newAdn in ADN-like record in EF
     *
     * getAdnRecordsInEf must be called at least once before this function,
     * otherwise an error will be returned. Currently the email field
     * if set in the ADN record is ignored.
     * throws SecurityException if no WRITE_CONTACTS permission
     *
     * @param efid must be one among EF_ADN, EF_FDN, and EF_SDN
     * @param oldTag adn tag to be replaced
     * @param oldPhoneNumber adn number to be replaced
     *        Set both oldTag and oldPhoneNubmer to "" means to replace an
     *        empty record, aka, insert new record
     * @param newTag adn tag to be stored
     * @param newPhoneNumber adn number ot be stored
     *        Set both newTag and newPhoneNubmer to "" means to replace the old
     *        record with empty one, aka, delete old record
     * @param pin2 required to update EF_FDN, otherwise must be null
     * @return true for success
     */
    /*public boolean
    updateAdnRecordsInEfBySearch (int efid,
            String oldTag, String oldPhoneNumber,
            String newTag, String newPhoneNumber, String pin2) {


        if (phone.getContext().checkCallingOrSelfPermission(
                android.Manifest.permission.WRITE_CONTACTS)
            != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(
                    "Requires android.permission.WRITE_CONTACTS permission");
        }
        int newid =   updateEfForIccType(efid);

	  int fileId[] = adnCache.getRecordsEfId(newid);

	  for(int i=0; i<fileId.length;i++){
	  	
            Log.i("IccPhoneBookInterfaceManager","updateAdnRecordsInEfBySearch: efid=" + fileId[i]);
	  }

       Log.i("IccPhoneBookInterfaceManager","updateAdnRecordsInEfBySearch: efid=" + efid +
                " ("+ oldTag + "," + oldPhoneNumber + ")"+ "==>" +
                " ("+ newTag + "," + newPhoneNumber + ")"+ " pin2=" + pin2);
        synchronized(mLock) {
            checkThread();
            success = false;
            Message response = mBaseHandler.obtainMessage(EVENT_UPDATE_DONE);
            AdnRecord oldAdn = new AdnRecord(oldTag, oldPhoneNumber);
            AdnRecord newAdn = new AdnRecord(newTag, newPhoneNumber);
	      Log.i("IccPhoneBookInterfaceManager", "newTag="+newTag+"  newPhoneNumber="+newPhoneNumber);
            for(int i=0; i<fileId.length;i++){
	  	   
                if (DBG) logd("addAdnRecordsInEf: efid=" + fileId[i]);
				
		   if(adnCache.updateAdnBySearch(fileId[i], oldAdn, newAdn, pin2, response)){

			  break;
		   }
	     }
            //adnCache.updateAdnBySearch(efid, oldAdn, newAdn, pin2, response);
            try {
                mLock.wait();
            } catch (InterruptedException e) {
                logd("interrupted while trying to update by search");
            }
        }
        return success;
    }*/

	//yeezone:jinwei Add a new contact in SimCard.
     /**Add newAdn in ADN-like record in EF	 
      * @param efid must be one among EF_ADN, EF_FDN, and EF_SDN	 
      * @param newTag adn tag to be stored     
      * @param newPhoneNumber adn number ot be stored     
      *        Set both newTag and newPhoneNubmer to "" means to replace the old     
      *        record with empty one, aka, delete old record     
      * @param pin2 required to update EF_FDN, otherwise must be null     
      * @return sim_index	
     */
    public boolean
    updateAdnRecordsInEfBySearch (int efid,
            String oldTag, String oldPhoneNumber,String[] oldEmailList,
            String newTag, String newPhoneNumber,String[] newEmailList,
            String newAnr,String newAas, String newSne, String newGrp, 
            String newGas,  String pin2) {
        if (phone.getContext().checkCallingOrSelfPermission(
                android.Manifest.permission.WRITE_CONTACTS)
            != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(
                    "Requires android.permission.WRITE_CONTACTS permission");
        }
        Log.v("IccPhoneBookInterfaceManager", "addIccRecordToEf** ");
        if (DBG) logd("addAdnRecordsInEf: efid=" + efid +
                " ("+ newTag + "," + newPhoneNumber + ")"+ " pin2=" + pin2);

	  int newid =   updateEfForIccType(efid);

        Log.i("IccPhoneBookInterfaceManager "," newid " +newid);
	 /* int fileId[] = adnCache.getRecordsEfId(newid);

	  for(int i=0; i<fileId.length;i++){
	  	
            if (DBG) logd("addAdnRecordsInEf: efid=" + fileId[i]);
	  }*/
   
        synchronized(mLock) {
            checkThread();
            Message response = mBaseHandler.obtainMessage(EVENT_UPDATE_DONE);
            AdnRecord oldAdn = null;
            AdnRecord newAdn = null;

            if (efid == IccConstants.EF_PBR) {
                oldAdn = new AdnRecord(oldTag, oldPhoneNumber,oldEmailList);
                newAdn = new AdnRecord(newTag, newPhoneNumber,newEmailList,newAnr,newAas,newSne,newGrp,newGas);
                adnCache.updateUSIMAdnBySearch(newid, oldAdn, newAdn, pin2, response);
            } else {
                oldAdn = new AdnRecord(oldTag, oldPhoneNumber);
                newAdn = new AdnRecord(newTag, newPhoneNumber);
                adnCache.updateAdnBySearch(newid, oldAdn, newAdn, pin2, response);
            }
       	 try {
                mLock.wait();
            } catch (InterruptedException e) {
                logd("interrupted while trying to update by search");
            }

	     }
	
         
	   
        return success;
    }
	//end Add a new contact in SimCard.

    /**
     * Update an ADN-like EF record by record index
     *
     * This is useful for iteration the whole ADN file, such as write the whole
     * phone book or erase/format the whole phonebook. Currently the email field
     * if set in the ADN record is ignored.
     * throws SecurityException if no WRITE_CONTACTS permission
     *
     * @param efid must be one among EF_ADN, EF_FDN, and EF_SDN
     * @param newTag adn tag to be stored
     * @param newPhoneNumber adn number to be stored
     *        Set both newTag and newPhoneNubmer to "" means to replace the old
     *        record with empty one, aka, delete old record
     * @param index is 1-based adn record index to be updated
     * @param pin2 required to update EF_FDN, otherwise must be null
     * @return true for success
     */
    public boolean updateAdnRecordsInEfByIndex(int efid, String newTag, String newPhoneNumber,
				List<String> newEmailList,String newAnr,String newAas, String newSne,String newGrp, String newGas,  int index, String pin2) {

        if (phone.getContext().checkCallingOrSelfPermission(
                android.Manifest.permission.WRITE_CONTACTS)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(
                    "Requires android.permission.WRITE_CONTACTS permission");
        }

			 if (DBG) logd("updateAdnRecordsInEfByIndex: efid=" + efid + " ( index= "+ index + ")"+ "==>" +
					" ("+ newTag + "," + newPhoneNumber + ","+ newAnr + "," + newSne +")"+ " pin2=" + pin2);
			 
			efid = updateEfForIccType(efid);
	
		String [] newEmails = null;
		if(newEmailList!=null){
				newEmails = newEmailList.toArray(new String[newEmailList.size()]);
		}
			synchronized(mSyncLock){
				logd("mSyncLock into");
			synchronized(mLock) {
				//checkThread();
				success = false;
				Message response = mBaseHandler.obtainMessage(EVENT_UPDATE_DONE);
				
				AdnRecord newAdn = null;
				if (efid == IccConstants.EF_PBR) {
					newAdn = new AdnRecord(newTag, newPhoneNumber,newEmails,newAnr,newAas,newSne,newGrp,newGas);
					adnCache.updateUSIMAdnByIndex(efid, newAdn, index, pin2, response);
				} else {
					newAdn = new AdnRecord(newTag, newPhoneNumber);
					adnCache.updateAdnByIndex(efid, newAdn, index, pin2, response);
				}
				try {
					mLock.wait();
				} catch (InterruptedException e) {
					logd("interrupted while trying to update by index");
				}
			}
			}
			logd("mSyncLock outo");
			return success;
		}
    /**
     * Get the capacity of records in efid
     *
     * @param efid the EF id of a ADN-like ICC
     * @return  int[3] array
     *            recordSizes[0]  is the single record length
     *            recordSizes[1]  is the total length of the EF file
     *            recordSizes[2]  is the number of records in the EF file
     */
    public  int[] getAdnRecordsSize(int efid){

          if (DBG) logd("getAdnRecordsSize: efid=" + efid);

	    int nefid = updateEfForIccType(efid);

	    if(nefid == IccConstants.EF_PBR){


	          recordSize = adnCache.getAdnRecordsSize(efid);
	    }else{
	    
               getRecordsSizeofAnd(efid);
	   }

        return recordSize;


    }

    private int[] getRecordsSizeofAnd(int efid) {
        if (DBG) logd("getAdnRecordsSize: efid=" + efid);
        synchronized(mLock) {
            checkThread();
            recordSize = new int[3];

            //Using mBaseHandler, no difference in EVENT_GET_SIZE_DONE handling
            Message response = mBaseHandler.obtainMessage(EVENT_GET_SIZE_DONE);

            phone.getIccFileHandler().getEFLinearRecordSize(efid, response);
            try {
                mLock.wait();
            } catch (InterruptedException e) {
                logd("interrupted while trying to load from the SIM");
            }
        }

        return recordSize;
    }

// zhanglj add begin for
    protected abstract int[] getEmailRecordsSize();
    protected abstract int getAnrCountsSize();

	public boolean hasEmailInIccCard(){
		return adnCache.mUsimPhoneBookManager.ishaveEmail;
	}

public boolean getIshaveAnrField(){
		return adnCache.mUsimPhoneBookManager.ishaveAnr;
	}
	
 public boolean getIshaveSneField(){
		return adnCache.mUsimPhoneBookManager.ishaveSne;
	}
    public int  getInsertIndex(){
		return adnCache.mInsetIndex;
	}
// zhanglj add end

    /**
     * Loads the AdnRecords in efid and returns them as a
     * List of AdnRecords
     *
     * throws SecurityException if no READ_CONTACTS permission
     *
     * @param efid the EF id of a ADN-like ICC
     * @return List of AdnRecord
     */
    public List<AdnRecord> getAdnRecordsInEf(int efid) {

        if (phone.getContext().checkCallingOrSelfPermission(
                android.Manifest.permission.READ_CONTACTS)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(
                    "Requires android.permission.READ_CONTACTS permission");
        }

        efid = updateEfForIccType(efid);
        if (DBG) logd("getAdnRecordsInEF: efid=" + efid);

        synchronized(mLock) {
            checkThread();
            Message response = mBaseHandler.obtainMessage(EVENT_LOAD_DONE);
            adnCache.requestLoadAllAdnLike(efid, adnCache.extensionEfForEf(efid), response);
            try {
                mLock.wait();
            } catch (InterruptedException e) {
                logd("interrupted while trying to load from the SIM");
            }
        }
            return records;
    }

    protected void checkThread() {
        if (!ALLOW_SIM_OP_IN_UI_THREAD) {
            // Make sure this isn't the UI thread, since it will block
            if (mBaseHandler.getLooper().equals(Looper.myLooper())) {
                loge("query() called on the main UI thread!");
                throw new IllegalStateException(
                        "You cannot call query on this provder from the main UI thread.");
            }
        }
    }

    private int updateEfForIccType(int efid) {
        // Check if we are trying to read ADN records
        if (efid == IccConstants.EF_ADN) {
            if (phone.getIccCard().isApplicationOnIcc(IccCardApplication.AppType.APPTYPE_USIM)) {
                return IccConstants.EF_PBR;
            }
        }
        return efid;
    }
}

